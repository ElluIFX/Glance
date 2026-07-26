#include "pch.h"
#include "shell_icon_provider.h"

#include <commoncontrols.h>
#include <robuffer.h>
#include <shellapi.h>
#include <shobjidl_core.h>
#include <wincodec.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace glance::app
{
    namespace
    {
        constexpr std::size_t maximum_cached_icons = 256;

        struct IconCacheKey
        {
            int image_list{};
            int icon_index{};
            std::uint32_t pixel_size{};

            bool operator==(const IconCacheKey&) const noexcept = default;
        };

        struct IconCacheKeyHash
        {
            std::size_t operator()(const IconCacheKey& key) const noexcept
            {
                std::size_t value = static_cast<std::size_t>(key.icon_index);
                value ^= static_cast<std::size_t>(key.image_list) << 24U;
                value ^= static_cast<std::size_t>(key.pixel_size) << 32U;
                return value;
            }
        };

        class unique_icon
        {
        public:
            unique_icon() noexcept = default;
            explicit unique_icon(HICON value) noexcept : value_(value) {}
            ~unique_icon()
            {
                reset();
            }

            unique_icon(const unique_icon&) = delete;
            unique_icon& operator=(const unique_icon&) = delete;

            [[nodiscard]] HICON get() const noexcept
            {
                return value_;
            }

            HICON* put() noexcept
            {
                reset();
                return &value_;
            }

            void reset(HICON value = nullptr) noexcept
            {
                if (value_ != nullptr)
                {
                    DestroyIcon(value_);
                }
                value_ = value;
            }

        private:
            HICON value_{};
        };

        class unique_bitmap
        {
        public:
            unique_bitmap() noexcept = default;
            explicit unique_bitmap(HBITMAP value) noexcept : value_(value) {}
            ~unique_bitmap()
            {
                reset();
            }

            unique_bitmap(const unique_bitmap&) = delete;
            unique_bitmap& operator=(const unique_bitmap&) = delete;

            [[nodiscard]] HBITMAP get() const noexcept
            {
                return value_;
            }

            HBITMAP* put() noexcept
            {
                reset();
                return &value_;
            }

            void reset(HBITMAP value = nullptr) noexcept
            {
                if (value_ != nullptr)
                {
                    DeleteObject(value_);
                }
                value_ = value;
            }

        private:
            HBITMAP value_{};
        };

        class scoped_com_apartment
        {
        public:
            scoped_com_apartment() noexcept
                : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED))
            {
            }

            ~scoped_com_apartment()
            {
                if (SUCCEEDED(result_))
                {
                    CoUninitialize();
                }
            }

            scoped_com_apartment(const scoped_com_apartment&) = delete;
            scoped_com_apartment& operator=(const scoped_com_apartment&) = delete;

        private:
            HRESULT result_{};
        };

        std::mutex cache_mutex;
        std::unordered_map<IconCacheKey, ShellIconBitmapPtr, IconCacheKeyHash> icon_cache;

        struct IconContentInspection
        {
            bool has_visible_pixels{};
            std::optional<WICRect> sparse_bounds;
        };

        int image_list_for_size(std::uint32_t pixel_size) noexcept
        {
            if (pixel_size <= 16)
            {
                return SHIL_SMALL;
            }
            if (pixel_size <= 32)
            {
                return SHIL_LARGE;
            }
            if (pixel_size <= 48)
            {
                return SHIL_EXTRALARGE;
            }
            return SHIL_JUMBO;
        }

        bool query_icon_index(
            const std::wstring& path,
            bool is_folder,
            bool use_file_attributes,
            int& icon_index) noexcept
        {
            SHFILEINFOW info{};
            UINT flags = SHGFI_SYSICONINDEX;
            DWORD attributes{};
            if (use_file_attributes)
            {
                flags |= SHGFI_USEFILEATTRIBUTES;
                attributes = is_folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            }
            if (SHGetFileInfoW(
                    path.c_str(),
                    attributes,
                    &info,
                    sizeof(info),
                    flags) == 0)
            {
                return false;
            }
            icon_index = info.iIcon;
            return true;
        }

        IconContentInspection inspect_icon_content(
            IWICImagingFactory* factory,
            IWICBitmapSource* source)
        {
            UINT width{};
            UINT height{};
            if (FAILED(source->GetSize(&width, &height)) || width == 0 || height == 0)
            {
                return {};
            }

            winrt::com_ptr<IWICFormatConverter> converter;
            if (FAILED(factory->CreateFormatConverter(converter.put())) ||
                FAILED(converter->Initialize(
                    source,
                    GUID_WICPixelFormat32bppPBGRA,
                    WICBitmapDitherTypeNone,
                    nullptr,
                    0.0,
                    WICBitmapPaletteTypeCustom)))
            {
                return {};
            }

            const UINT stride = width * 4U;
            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * height);
            if (FAILED(converter->CopyPixels(
                    nullptr,
                    stride,
                    static_cast<UINT>(pixels.size()),
                    pixels.data())))
            {
                return {};
            }

            UINT left = width;
            UINT top = height;
            UINT right{};
            UINT bottom{};
            for (UINT y = 0; y < height; ++y)
            {
                for (UINT x = 0; x < width; ++x)
                {
                    const auto alpha = pixels[static_cast<std::size_t>(y) * stride + x * 4U + 3U];
                    if (alpha == 0)
                    {
                        continue;
                    }
                    left = std::min(left, x);
                    top = std::min(top, y);
                    right = std::max(right, x + 1U);
                    bottom = std::max(bottom, y + 1U);
                }
            }
            if (left >= right || top >= bottom)
            {
                return {};
            }

            const UINT content_width = right - left;
            const UINT content_height = bottom - top;
            if (std::max(content_width, content_height) * 4U >= std::max(width, height) * 3U)
            {
                return IconContentInspection{ true, std::nullopt };
            }
            return IconContentInspection{
                true,
                WICRect{
                    static_cast<INT>(left),
                    static_cast<INT>(top),
                    static_cast<INT>(content_width),
                    static_cast<INT>(content_height) } };
        }

        ShellIconBitmapPtr convert_icon(HICON icon, std::uint32_t pixel_size)
        {
            winrt::com_ptr<IWICImagingFactory> factory;
            if (FAILED(CoCreateInstance(
                    CLSID_WICImagingFactory2,
                    nullptr,
                    CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(factory.put()))))
            {
                return nullptr;
            }

            winrt::com_ptr<IWICBitmap> source;
            if (FAILED(factory->CreateBitmapFromHICON(icon, source.put())))
            {
                return nullptr;
            }

            IWICBitmapSource* render_source = source.get();
            winrt::com_ptr<IWICBitmapClipper> clipper;
            UINT render_width = pixel_size;
            UINT render_height = pixel_size;
            bool center_scaled_content{};
            const auto content = inspect_icon_content(factory.get(), source.get());
            if (!content.has_visible_pixels)
            {
                return nullptr;
            }
            if (pixel_size >= 48 && content.sparse_bounds.has_value())
            {
                const auto& bounds = content.sparse_bounds.value();
                if (SUCCEEDED(factory->CreateBitmapClipper(clipper.put())) &&
                    SUCCEEDED(clipper->Initialize(source.get(), &bounds)))
                {
                    render_source = clipper.get();
                    const UINT extent = std::max(1U, pixel_size * 7U / 8U);
                    const UINT content_width = static_cast<UINT>(bounds.Width);
                    const UINT content_height = static_cast<UINT>(bounds.Height);
                    if (content_width >= content_height)
                    {
                        render_width = extent;
                        render_height = std::max(
                            1U,
                            static_cast<UINT>((
                                static_cast<std::uint64_t>(content_height) * extent +
                                content_width / 2U) / content_width));
                    }
                    else
                    {
                        render_height = extent;
                        render_width = std::max(
                            1U,
                            static_cast<UINT>((
                                static_cast<std::uint64_t>(content_width) * extent +
                                content_height / 2U) / content_height));
                    }
                    center_scaled_content = true;
                }
            }

            winrt::com_ptr<IWICBitmapScaler> scaler;
            if (FAILED(factory->CreateBitmapScaler(scaler.put())) ||
                FAILED(scaler->Initialize(
                    render_source,
                    render_width,
                    render_height,
                    WICBitmapInterpolationModeFant)))
            {
                return nullptr;
            }

            winrt::com_ptr<IWICFormatConverter> converter;
            if (FAILED(factory->CreateFormatConverter(converter.put())) ||
                FAILED(converter->Initialize(
                    scaler.get(),
                    GUID_WICPixelFormat32bppPBGRA,
                    WICBitmapDitherTypeNone,
                    nullptr,
                    0.0,
                    WICBitmapPaletteTypeCustom)))
            {
                return nullptr;
            }

            const auto stride = pixel_size * 4U;
            const auto byte_count = stride * pixel_size;
            auto result = std::make_shared<ShellIconBitmap>();
            result->width = pixel_size;
            result->height = pixel_size;
            result->pixels.assign(byte_count, 0);
            if (!center_scaled_content)
            {
                if (FAILED(converter->CopyPixels(
                        nullptr,
                        stride,
                        byte_count,
                        result->pixels.data())))
                {
                    return nullptr;
                }
                return result;
            }

            const UINT render_stride = render_width * 4U;
            const UINT render_byte_count = render_stride * render_height;
            std::vector<std::uint8_t> rendered(render_byte_count);
            if (FAILED(converter->CopyPixels(
                    nullptr,
                    render_stride,
                    render_byte_count,
                    rendered.data())))
            {
                return nullptr;
            }
            const UINT offset_x = (pixel_size - render_width) / 2U;
            const UINT offset_y = (pixel_size - render_height) / 2U;
            for (UINT y = 0; y < render_height; ++y)
            {
                std::memcpy(
                    result->pixels.data() +
                        static_cast<std::size_t>(offset_y + y) * stride + offset_x * 4U,
                    rendered.data() + static_cast<std::size_t>(y) * render_stride,
                    render_stride);
            }
            return result;
        }

        ShellIconBitmapPtr convert_thumbnail(HBITMAP bitmap)
        {
            winrt::com_ptr<IWICImagingFactory> factory;
            if (FAILED(CoCreateInstance(
                    CLSID_WICImagingFactory2,
                    nullptr,
                    CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(factory.put()))))
            {
                return nullptr;
            }

            winrt::com_ptr<IWICBitmap> source;
            if (FAILED(factory->CreateBitmapFromHBITMAP(
                    bitmap,
                    nullptr,
                    WICBitmapUsePremultipliedAlpha,
                    source.put())))
            {
                return nullptr;
            }

            UINT width{};
            UINT height{};
            if (FAILED(source->GetSize(&width, &height)) || width == 0 || height == 0)
            {
                return nullptr;
            }

            winrt::com_ptr<IWICFormatConverter> converter;
            if (FAILED(factory->CreateFormatConverter(converter.put())) ||
                FAILED(converter->Initialize(
                    source.get(),
                    GUID_WICPixelFormat32bppPBGRA,
                    WICBitmapDitherTypeNone,
                    nullptr,
                    0.0,
                    WICBitmapPaletteTypeCustom)))
            {
                return nullptr;
            }

            const UINT stride = width * 4U;
            const UINT byte_count = stride * height;
            auto result = std::make_shared<ShellIconBitmap>();
            result->width = width;
            result->height = height;
            result->pixels.resize(byte_count);
            if (FAILED(converter->CopyPixels(
                    nullptr,
                    stride,
                    byte_count,
                    result->pixels.data())))
            {
                return nullptr;
            }
            bool has_visible_pixels{};
            for (std::size_t index = 3; index < result->pixels.size(); index += 4)
            {
                if (result->pixels[index] != 0)
                {
                    has_visible_pixels = true;
                    break;
                }
            }
            if (!has_visible_pixels)
            {
                return nullptr;
            }
            return result;
        }
    }

    ShellIconBitmapPtr load_shell_icon(
        std::wstring_view path,
        bool is_folder,
        std::uint32_t pixel_size,
        bool use_file_attributes) noexcept try
    {
        if (path.empty() || pixel_size == 0)
        {
            return nullptr;
        }

        const scoped_com_apartment apartment;
        const std::wstring path_value(path);
        int icon_index{};
        if (!query_icon_index(path_value, is_folder, use_file_attributes, icon_index) &&
            (use_file_attributes || !query_icon_index(path_value, is_folder, true, icon_index)))
        {
            return nullptr;
        }

        const int image_list = image_list_for_size(pixel_size);
        const IconCacheKey key{ image_list, icon_index, pixel_size };
        {
            const std::scoped_lock lock(cache_mutex);
            if (const auto iterator = icon_cache.find(key); iterator != icon_cache.end())
            {
                return iterator->second;
            }
        }

        winrt::com_ptr<IImageList> system_image_list;
        if (FAILED(SHGetImageList(
                image_list,
                IID_IImageList,
                system_image_list.put_void())))
        {
            return nullptr;
        }

        unique_icon icon;
        if (FAILED(system_image_list->GetIcon(icon_index, ILD_TRANSPARENT, icon.put())))
        {
            return nullptr;
        }

        auto bitmap = convert_icon(icon.get(), pixel_size);
        if (bitmap == nullptr)
        {
            return nullptr;
        }

        const std::scoped_lock lock(cache_mutex);
        if (icon_cache.size() >= maximum_cached_icons)
        {
            icon_cache.erase(icon_cache.begin());
        }
        const auto iterator = icon_cache.emplace(key, std::move(bitmap)).first;
        return iterator->second;
    }
    catch (...)
    {
        return nullptr;
    }

    ShellIconBitmapPtr load_shell_thumbnail(
        std::wstring_view path,
        std::uint32_t pixel_size) noexcept try
    {
        if (path.empty() || pixel_size == 0 ||
            pixel_size > static_cast<std::uint32_t>(std::numeric_limits<LONG>::max()))
        {
            return nullptr;
        }

        const scoped_com_apartment apartment;
        winrt::com_ptr<IShellItemImageFactory> image_factory;
        const std::wstring path_value(path);
        if (FAILED(SHCreateItemFromParsingName(
                path_value.c_str(),
                nullptr,
                IID_PPV_ARGS(image_factory.put()))))
        {
            return nullptr;
        }

        unique_bitmap bitmap;
        const SIZE size{
            static_cast<LONG>(pixel_size),
            static_cast<LONG>(pixel_size) };
        if (FAILED(image_factory->GetImage(
                size,
                SIIGBF_THUMBNAILONLY,
                bitmap.put())))
        {
            return nullptr;
        }
        return convert_thumbnail(bitmap.get());
    }
    catch (...)
    {
        return nullptr;
    }

    winrt::Microsoft::UI::Xaml::Media::ImageSource create_shell_icon_source(
        const ShellIconBitmap& bitmap)
    {
        if (bitmap.width == 0 || bitmap.height == 0 || bitmap.pixels.empty())
        {
            return nullptr;
        }
        bool has_visible_pixels{};
        for (std::size_t index = 3; index < bitmap.pixels.size(); index += 4)
        {
            if (bitmap.pixels[index] != 0)
            {
                has_visible_pixels = true;
                break;
            }
        }
        if (!has_visible_pixels)
        {
            return nullptr;
        }

        winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap source(
            static_cast<std::int32_t>(bitmap.width),
            static_cast<std::int32_t>(bitmap.height));
        const auto buffer = source.PixelBuffer();
        const auto byte_access = buffer.as<::Windows::Storage::Streams::IBufferByteAccess>();
        BYTE* destination{};
        winrt::check_hresult(byte_access->Buffer(&destination));
        std::memcpy(destination, bitmap.pixels.data(), bitmap.pixels.size());
        return source;
    }
}

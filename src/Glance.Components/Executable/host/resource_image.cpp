#include "resource_image.h"
#include <objidl.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <stdexcept>

namespace glance::executable
{
    namespace
    {
        struct Image
        {
            Microsoft::WRL::ComPtr<IStream> stream;
            std::unique_ptr<Gdiplus::Bitmap> bitmap;
            HICON icon{};
            ~Image()
            {
                if (icon)
                    DestroyIcon(icon);
            }
            Image(const std::wstring& path, const Identity& identity, const Row& row)
            {
                if (row.length > 16 * 1024 * 1024)
                    throw std::runtime_error("Partial");
                auto data = (row.type == 14 || row.type == 12)
                                ? icon_file(path, identity, row)
                                : read_resource(path, identity, row, row.length);
                if (row.type == 3 || row.type == 1 || row.type == 14 || row.type == 12)
                {
                    if (row.type == 14 || row.type == 12)
                    {
                        if (data.size() < 22)
                            throw std::runtime_error("Invalid");
                        DWORD length{}, offset{};
                        WORD count{};
                        memcpy(&count, data.data() + 4, 2);
                        if (data.size() < 6 + count * 16ULL)
                            throw std::runtime_error("Invalid");
                        unsigned selected{}, largest{};
                        for (unsigned i = 0; i < count; ++i)
                        {
                            const auto width = std::to_integer<unsigned>(data[6 + i * 16]);
                            if ((width ? width : 256) > largest)
                            {
                                largest = width ? width : 256;
                                selected = i;
                            }
                        }
                        memcpy(&length, data.data() + 14 + selected * 16, 4);
                        memcpy(&offset, data.data() + 18 + selected * 16, 4);
                        if (offset > data.size() || length > data.size() - offset)
                            throw std::runtime_error("Invalid");
                        data = {data.begin() + offset, data.begin() + offset + length};
                    }
                    else if (row.type == 1)
                    {
                        if (data.size() < 4)
                            throw std::runtime_error("Invalid");
                        data.erase(data.begin(), data.begin() + 4);
                    }
                    icon = CreateIconFromResourceEx(reinterpret_cast<PBYTE>(data.data()),
                                                    static_cast<DWORD>(data.size()), TRUE, 0x00030000, 128,
                                                    128, LR_DEFAULTCOLOR);
                    if (icon)
                        bitmap.reset(Gdiplus::Bitmap::FromHICON(icon));
                }
                else
                {
                    if (row.type == 2)
                    {
                        if (data.size() < sizeof(BITMAPINFOHEADER))
                            throw std::runtime_error("Invalid");
                        BITMAPINFOHEADER header{};
                        memcpy(&header, data.data(), sizeof(header));
                        if (header.biSize < 40 || header.biSize > data.size() || header.biWidth <= 0 ||
                            header.biWidth > 8192 || header.biHeight == 0 || header.biHeight < -8192 ||
                            header.biHeight > 8192)
                            throw std::runtime_error("Invalid");
                        const auto colors = header.biClrUsed         ? header.biClrUsed
                                            : header.biBitCount <= 8 ? (1U << header.biBitCount)
                                                                     : 0;
                        const auto offset =
                            14ULL + header.biSize + colors * 4ULL +
                            (header.biSize == 40 && header.biCompression == BI_BITFIELDS ? 12 : 0);
                        if (offset > data.size() + 14)
                            throw std::runtime_error("Invalid");
                        BITMAPFILEHEADER file{};
                        file.bfType = 0x4d42;
                        file.bfSize = static_cast<DWORD>(data.size() + 14);
                        file.bfOffBits = static_cast<DWORD>(offset);
                        data.insert(data.begin(), 14, std::byte{});
                        memcpy(data.data(), &file, 14);
                    }
                    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(data.data()),
                                                    static_cast<UINT>(data.size())));
                    if (stream)
                        bitmap.reset(Gdiplus::Bitmap::FromStream(stream.Get()));
                }
                if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok || bitmap->GetWidth() > 8192 ||
                    bitmap->GetHeight() > 8192)
                    throw std::runtime_error("Invalid");
            }
        };
    } // namespace
    ResourceImage resource_image(const std::wstring& path, const Identity& identity, const Row& row)
    {
        Image image(path, identity, row);
        const auto width = image.bitmap->GetWidth(), height = image.bitmap->GetHeight();
        if (!width || !height)
            throw std::runtime_error("Invalid");
        const double scale = std::min(1.0, 512.0 / std::max(width, height));
        ResourceImage result;
        result.width = std::max(1, static_cast<int>(width * scale));
        result.height = std::max(1, static_cast<int>(height * scale));
        Gdiplus::Bitmap thumbnail(result.width, result.height, PixelFormat32bppARGB);
        Gdiplus::Graphics graphics(&thumbnail);
        graphics.Clear(Gdiplus::Color(255, 245, 245, 245));
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.DrawImage(image.bitmap.get(), 0, 0, result.width, result.height);
        HBITMAP bitmap{};
        if (thumbnail.GetHBITMAP(Gdiplus::Color(255, 245, 245, 245), &bitmap) != Gdiplus::Ok)
            throw std::runtime_error("Invalid");
        result.bitmap = {bitmap, [](void* handle) { DeleteObject(handle); }};
        return result;
    }
    void save_image(const std::wstring& path, const Identity& identity, const Row& row,
                    const std::wstring& destination)
    {
        Image image(path, identity, row);
        const CLSID png{0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
        if (image.bitmap->Save(destination.c_str(), &png, nullptr) != Gdiplus::Ok)
            throw std::runtime_error("SaveFailed");
    }
} // namespace glance::executable

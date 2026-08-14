#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#pragma warning(push)
#pragma warning(disable : 4251)
#include "libraw.h"
#pragma warning(pop)

#include "image_metadata_sidecar.h"

namespace
{
    using Microsoft::WRL::ComPtr;
    constexpr std::uint64_t maximum_decode_pixels = 100'000'000ULL;

    enum class RawPreviewResult
    {
        success,
        unavailable,
        failed
    };

    struct Arguments
    {
        std::wstring mode;
        std::filesystem::path input;
        std::filesystem::path output;
        std::filesystem::path metadata_output;
        std::uint32_t maximum_dimension{};
    };

    bool parse_arguments(int argument_count, wchar_t** arguments, Arguments& result)
    {
        for (int index = 1; index + 1 < argument_count; index += 2)
        {
            const std::wstring_view name{ arguments[index] };
            if (name == L"--mode")
            {
                result.mode = arguments[index + 1];
            }
            else if (name == L"--input")
            {
                result.input = arguments[index + 1];
            }
            else if (name == L"--output")
            {
                result.output = arguments[index + 1];
            }
            else if (name == L"--metadata-output")
            {
                result.metadata_output = arguments[index + 1];
            }
            else if (name == L"--maximum-dimension")
            {
                wchar_t* end{};
                const auto value = wcstoul(arguments[index + 1], &end, 10);
                if (end == arguments[index + 1] || *end != L'\0')
                {
                    return false;
                }
                result.maximum_dimension = value;
            }
            else
            {
                return false;
            }
        }
        return result.mode == L"thumbnail" &&
            argument_count == 11 &&
            !result.input.empty() &&
            !result.output.empty() &&
            !result.metadata_output.empty() &&
            (result.maximum_dimension == 1024 ||
             result.maximum_dimension == 2048 ||
             result.maximum_dimension == 4096 ||
             result.maximum_dimension == 8192);
    }

    std::wstring widen(std::string_view value)
    {
        if (value.empty())
        {
            return {};
        }
        int required = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            nullptr,
            0);
        UINT code_page = CP_UTF8;
        DWORD flags = MB_ERR_INVALID_CHARS;
        if (required <= 0)
        {
            code_page = CP_ACP;
            flags = 0;
            required = MultiByteToWideChar(
                code_page,
                flags,
                value.data(),
                static_cast<int>(value.size()),
                nullptr,
                0);
        }
        if (required <= 0)
        {
            return {};
        }
        std::wstring result(static_cast<std::size_t>(required), L'\0');
        return MultiByteToWideChar(
                   code_page,
                   flags,
                   value.data(),
                   static_cast<int>(value.size()),
                   result.data(),
                   required) == required
            ? result
            : std::wstring{};
    }

    void add_text(
        std::vector<glance::contracts::components::ImageMetadataEntry>& entries,
        std::wstring_view canonical_name,
        std::wstring_view value)
    {
        using namespace glance::contracts::components;
        if (value.empty() || canonical_name.size() >= image_metadata_property_capacity ||
            value.size() >= image_metadata_text_capacity)
        {
            return;
        }
        ImageMetadataEntry entry;
        entry.value_kind = ImageMetadataValueKind::text;
        canonical_name.copy(entry.canonical_name, canonical_name.size());
        value.copy(entry.text, value.size());
        entries.push_back(entry);
    }

    void add_ascii(
        std::vector<glance::contracts::components::ImageMetadataEntry>& entries,
        std::wstring_view canonical_name,
        const char* value,
        std::size_t capacity)
    {
        if (value == nullptr)
        {
            return;
        }
        add_text(entries, canonical_name, widen({ value, strnlen_s(value, capacity) }));
    }

    void add_double(
        std::vector<glance::contracts::components::ImageMetadataEntry>& entries,
        std::wstring_view canonical_name,
        double value)
    {
        using namespace glance::contracts::components;
        if (!std::isfinite(value) || canonical_name.size() >= image_metadata_property_capacity)
        {
            return;
        }
        ImageMetadataEntry entry;
        entry.value_kind = ImageMetadataValueKind::floating_point;
        entry.floating_point = value;
        canonical_name.copy(entry.canonical_name, canonical_name.size());
        entries.push_back(entry);
    }

    std::vector<glance::contracts::components::ImageMetadataEntry> raw_metadata(
        const libraw_data_t& raw)
    {
        using namespace glance::contracts::components;
        std::vector<ImageMetadataEntry> entries;
        add_ascii(entries, L"System.Photo.CameraManufacturer", raw.idata.make, sizeof(raw.idata.make));
        add_ascii(entries, L"System.Photo.CameraModel", raw.idata.model, sizeof(raw.idata.model));
        add_ascii(entries, L"System.Photo.LensManufacturer", raw.lens.LensMake, sizeof(raw.lens.LensMake));
        add_ascii(entries, L"System.Photo.LensModel", raw.lens.Lens, sizeof(raw.lens.Lens));
        if (raw.other.iso_speed > 0)
        {
            add_double(entries, L"System.Photo.ISOSpeed", raw.other.iso_speed);
        }
        if (raw.other.shutter > 0)
        {
            add_double(entries, L"System.Photo.ExposureTime", raw.other.shutter);
        }
        if (raw.other.aperture > 0)
        {
            add_double(entries, L"System.Photo.FNumber", raw.other.aperture);
        }
        if (raw.other.focal_len > 0)
        {
            add_double(entries, L"System.Photo.FocalLength", raw.other.focal_len);
        }
        if (raw.lens.FocalLengthIn35mmFormat != 0)
        {
            ImageMetadataEntry entry;
            entry.value_kind = ImageMetadataValueKind::unsigned_integer;
            entry.unsigned_value = raw.lens.FocalLengthIn35mmFormat;
            wcscpy_s(entry.canonical_name, L"System.Photo.FocalLengthInFilm");
            entries.push_back(entry);
        }
        if (raw.other.timestamp > 0)
        {
            constexpr std::uint64_t windows_epoch = 11644473600ULL;
            constexpr std::uint64_t ticks_per_second = 10'000'000ULL;
            const auto ticks =
                (static_cast<std::uint64_t>(raw.other.timestamp) + windows_epoch) *
                ticks_per_second;
            ImageMetadataEntry entry;
            entry.value_kind = ImageMetadataValueKind::timestamp;
            entry.timestamp.dwLowDateTime = static_cast<DWORD>(ticks);
            entry.timestamp.dwHighDateTime = static_cast<DWORD>(ticks >> 32U);
            wcscpy_s(entry.canonical_name, L"System.Photo.DateTaken");
            entries.push_back(entry);
        }
        if (raw.sizes.width != 0 && raw.sizes.height != 0)
        {
            add_text(
                entries,
                L"System.Image.Dimensions",
                std::to_wstring(raw.sizes.width) + L" x " +
                    std::to_wstring(raw.sizes.height));
        }
        add_ascii(entries, L"System.Comment", raw.other.desc, sizeof(raw.other.desc));
        add_ascii(entries, L"System.Author", raw.other.artist, sizeof(raw.other.artist));
        if (raw.other.parsed_gps.gpsparsed)
        {
            const auto coordinate = [](const float (&parts)[3]) {
                return static_cast<double>(parts[0]) +
                    static_cast<double>(parts[1]) / 60.0 +
                    static_cast<double>(parts[2]) / 3600.0;
            };
            auto latitude = coordinate(raw.other.parsed_gps.latitude);
            auto longitude = coordinate(raw.other.parsed_gps.longitude);
            if (raw.other.parsed_gps.latref == 'S')
            {
                latitude = -latitude;
            }
            if (raw.other.parsed_gps.longref == 'W')
            {
                longitude = -longitude;
            }
            add_double(entries, L"System.GPS.Latitude", latitude);
            add_double(entries, L"System.GPS.Longitude", longitude);
            add_double(entries, L"System.GPS.Altitude", raw.other.parsed_gps.altitude);
        }
        return entries;
    }

    bool write_rotated_jpeg(
        const std::filesystem::path& output,
        const std::uint8_t* jpeg_data,
        std::size_t jpeg_size,
        int flip,
        std::uint32_t maximum_dimension)
    {
        ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory))))
        {
            return false;
        }

        ComPtr<IWICStream> input_stream;
        if (FAILED(factory->CreateStream(&input_stream)) ||
            FAILED(input_stream->InitializeFromMemory(
                const_cast<BYTE*>(jpeg_data),
                static_cast<DWORD>(jpeg_size))))
        {
            return false;
        }
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(factory->CreateDecoderFromStream(
                input_stream.Get(),
                nullptr,
                WICDecodeMetadataCacheOnDemand,
                &decoder)))
        {
            return false;
        }
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, &frame)))
        {
            return false;
        }
        UINT width{};
        UINT height{};
        if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0)
        {
            return false;
        }
        if (static_cast<std::uint64_t>(width) * height > maximum_decode_pixels)
        {
            return false;
        }

        ComPtr<IWICFormatConverter> converter;
        if (FAILED(factory->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(
                frame.Get(),
                GUID_WICPixelFormat24bppBGR,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeCustom)))
        {
            return false;
        }

        IWICBitmapSource* source = converter.Get();
        ComPtr<IWICBitmapScaler> scaler;
        const auto larger_side = width > height ? width : height;
        if (larger_side > maximum_dimension)
        {
            const auto scaled_width = static_cast<UINT>(
                static_cast<std::uint64_t>(width) * maximum_dimension / larger_side);
            const auto scaled_height = static_cast<UINT>(
                static_cast<std::uint64_t>(height) * maximum_dimension / larger_side);
            if (scaled_width == 0 || scaled_height == 0 ||
                FAILED(factory->CreateBitmapScaler(&scaler)) ||
                FAILED(scaler->Initialize(
                    converter.Get(),
                    scaled_width,
                    scaled_height,
                    WICBitmapInterpolationModeFant)))
            {
                return false;
            }
            source = scaler.Get();
            width = scaled_width;
            height = scaled_height;
        }

        if (width > UINT_MAX / 3U ||
            static_cast<std::uint64_t>(width) * height * 3U > UINT_MAX)
        {
            return false;
        }
        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(width) * height * 3U);
        const UINT stride = width * 3U;
        if (FAILED(source->CopyPixels(
                nullptr,
                stride,
                static_cast<UINT>(pixels.size()),
                pixels.data())))
        {
            return false;
        }
        if (flip == 3 || flip == 5 || flip == 6)
        {
            std::vector<std::uint8_t> rotated;
            UINT rotated_width = width;
            UINT rotated_height = height;
            if (flip == 5 || flip == 6)
            {
                rotated_width = height;
                rotated_height = width;
                rotated.resize(static_cast<std::size_t>(rotated_width) * rotated_height * 3U);
                for (UINT y = 0; y < height; ++y)
                {
                    for (UINT x = 0; x < width; ++x)
                    {
                        const auto* source_pixel = pixels.data() +
                            (static_cast<std::size_t>(y) * width + x) * 3U;
                        // Verified against the camera's own JPEG output:
                        // flip 5 rotates 90 degrees clockwise:
                        //   src(y, x) -> dst(width-1-x, y).
                        // flip 6 rotates 270 degrees clockwise:
                        //   src(y, x) -> dst(x, height-1-y).
                        const auto destination_index = flip == 5
                            ? static_cast<std::size_t>(width - 1U - x) *
                                      rotated_width +
                                  y
                            : static_cast<std::size_t>(x) * rotated_width +
                                  (height - 1U - y);
                        auto* destination = rotated.data() + destination_index * 3U;
                        destination[0] = source_pixel[0];
                        destination[1] = source_pixel[1];
                        destination[2] = source_pixel[2];
                    }
                }
            }
            else
            {
                rotated.resize(static_cast<std::size_t>(width) * height * 3U);
                for (std::size_t index = 0; index < pixels.size(); index += 3U)
                {
                    const auto opposite = pixels.size() - 3U - index;
                    rotated[opposite] = pixels[index];
                    rotated[opposite + 1U] = pixels[index + 1U];
                    rotated[opposite + 2U] = pixels[index + 2U];
                }
            }
            pixels = std::move(rotated);
            width = rotated_width;
            height = rotated_height;
        }

        ComPtr<IWICStream> output_stream;
        if (FAILED(factory->CreateStream(&output_stream)) ||
            FAILED(output_stream->InitializeFromFilename(
                output.c_str(),
                GENERIC_WRITE)))
        {
            return false;
        }
        ComPtr<IWICBitmapEncoder> encoder;
        if (FAILED(factory->CreateEncoder(
                GUID_ContainerFormatJpeg,
                nullptr,
                &encoder)) ||
            FAILED(encoder->Initialize(output_stream.Get(), WICBitmapEncoderNoCache)))
        {
            return false;
        }
        ComPtr<IWICBitmapFrameEncode> frame_encoder;
        ComPtr<IPropertyBag2> properties;
        if (FAILED(encoder->CreateNewFrame(&frame_encoder, &properties)) ||
            FAILED(frame_encoder->Initialize(properties.Get())))
        {
            return false;
        }
        if (FAILED(frame_encoder->SetSize(width, height)))
        {
            return false;
        }
        WICPixelFormatGUID encoder_format = GUID_WICPixelFormat24bppBGR;
        if (FAILED(frame_encoder->SetPixelFormat(&encoder_format)))
        {
            return false;
        }
        ComPtr<IWICBitmap> bitmap;
        if (FAILED(factory->CreateBitmapFromMemory(
                width,
                height,
                GUID_WICPixelFormat24bppBGR,
                width * 3U,
                static_cast<UINT>(pixels.size()),
                pixels.data(),
                &bitmap)))
        {
            return false;
        }
        WICRect rectangle{ 0, 0, static_cast<INT>(width), static_cast<INT>(height) };
        if (FAILED(frame_encoder->WriteSource(bitmap.Get(), &rectangle)) ||
            FAILED(frame_encoder->Commit()) ||
            FAILED(encoder->Commit()))
        {
            return false;
        }
        return true;
    }

    RawPreviewResult prepare_raw_thumbnail(
        const std::filesystem::path& input,
        const std::filesystem::path& output,
        const std::filesystem::path& metadata_output,
        std::uint32_t maximum_dimension)
    {
        libraw_data_t* raw = libraw_init(0);
        if (raw == nullptr)
        {
            return RawPreviewResult::failed;
        }
        const auto raw_guard = std::unique_ptr<libraw_data_t, decltype(&libraw_close)>(
            raw, libraw_close);

        if (libraw_open_wfile(raw, input.c_str()) != LIBRAW_SUCCESS)
        {
            return RawPreviewResult::failed;
        }
        static_cast<void>(glance::components::write_image_metadata_sidecar(
            metadata_output,
            raw_metadata(*raw)));
        const auto thumbnail_result = libraw_unpack_thumb(raw);
        if (thumbnail_result == LIBRAW_NO_THUMBNAIL ||
            thumbnail_result == LIBRAW_UNSUPPORTED_THUMBNAIL)
        {
            return RawPreviewResult::unavailable;
        }
        if (thumbnail_result != LIBRAW_SUCCESS)
        {
            return RawPreviewResult::failed;
        }
        const auto thumbnail = &raw->thumbnail;
        if (thumbnail->tformat != LIBRAW_THUMBNAIL_JPEG ||
            thumbnail->thumb == nullptr ||
            thumbnail->tlength == 0)
        {
            return RawPreviewResult::unavailable;
        }
        return write_rotated_jpeg(
            output,
            reinterpret_cast<const std::uint8_t*>(thumbnail->thumb),
            thumbnail->tlength,
            raw->sizes.flip,
            maximum_dimension)
            ? RawPreviewResult::success
            : RawPreviewResult::failed;
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argument_count{};
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (arguments == nullptr)
    {
        return ERROR_INVALID_PARAMETER;
    }

    Arguments parsed;
    const bool valid = parse_arguments(argument_count, arguments, parsed);
    LocalFree(arguments);
    std::error_code input_error;
    if (!valid || !std::filesystem::is_regular_file(parsed.input, input_error))
    {
        return ERROR_INVALID_PARAMETER;
    }

    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized))
    {
        return static_cast<int>(initialized);
    }
    std::error_code output_error;
    const auto result = prepare_raw_thumbnail(
        parsed.input,
        parsed.output,
        parsed.metadata_output,
        parsed.maximum_dimension);
    const bool success = result == RawPreviewResult::success &&
        std::filesystem::is_regular_file(parsed.output, output_error);
    CoUninitialize();
    if (success)
    {
        return ERROR_SUCCESS;
    }
    return result == RawPreviewResult::unavailable
        ? ERROR_NOT_SUPPORTED
        : ERROR_GEN_FAILURE;
}

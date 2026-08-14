#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>

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
            argument_count == 9 &&
            !result.input.empty() &&
            !result.output.empty() &&
            (result.maximum_dimension == 1024 ||
             result.maximum_dimension == 2048 ||
             result.maximum_dimension == 4096 ||
             result.maximum_dimension == 8192);
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

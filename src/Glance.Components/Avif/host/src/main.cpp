#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <filesystem>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "avif.h"

namespace
{
    using Microsoft::WRL::ComPtr;
    constexpr std::uint32_t maximum_decode_pixels = 100'000'000U;
    constexpr std::uint32_t maximum_decode_dimension = 32768U;
    constexpr int maximum_decoder_threads = 4;

    struct Arguments
    {
        std::filesystem::path input;
        std::filesystem::path output;
        std::uint32_t maximum_dimension{};
    };

    bool parse_arguments(int argument_count, wchar_t** arguments, Arguments& result)
    {
        for (int index = 1; index + 1 < argument_count; index += 2)
        {
            const std::wstring_view name{ arguments[index] };
            if (name == L"--input")
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
        return argument_count == 7 &&
            !result.input.empty() &&
            !result.output.empty() &&
            (result.maximum_dimension == 1024 ||
             result.maximum_dimension == 2048 ||
             result.maximum_dimension == 4096 ||
             result.maximum_dimension == 8192);
    }

    bool write_png(
        const std::filesystem::path& output,
        const std::uint8_t* pixels,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t stride,
        bool has_alpha)
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
        ComPtr<IWICStream> stream;
        if (FAILED(factory->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromFilename(
                output.c_str(),
                GENERIC_WRITE)))
        {
            return false;
        }
        ComPtr<IWICBitmapEncoder> encoder;
        if (FAILED(factory->CreateEncoder(
                GUID_ContainerFormatPng,
                nullptr,
                &encoder)) ||
            FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
        {
            return false;
        }
        ComPtr<IWICBitmapFrameEncode> frame;
        if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) ||
            FAILED(frame->Initialize(nullptr)) ||
            FAILED(frame->SetSize(width, height)))
        {
            return false;
        }
        WICPixelFormatGUID source_format = has_alpha
            ? GUID_WICPixelFormat32bppBGRA
            : GUID_WICPixelFormat24bppRGB;
        if (FAILED(frame->SetPixelFormat(&source_format)))
        {
            return false;
        }
        ComPtr<IWICBitmap> bitmap;
        if (FAILED(factory->CreateBitmapFromMemory(
                width,
                height,
                source_format,
                stride,
                static_cast<UINT>(static_cast<std::uint64_t>(stride) * height),
                const_cast<std::uint8_t*>(pixels),
                &bitmap)))
        {
            return false;
        }
        WICRect rectangle{ 0, 0, static_cast<INT>(width), static_cast<INT>(height) };
        if (FAILED(frame->WriteSource(bitmap.Get(), &rectangle)) ||
            FAILED(frame->Commit()) ||
            FAILED(encoder->Commit()))
        {
            return false;
        }
        return true;
    }

    void transform_pixels(
        std::vector<std::uint8_t>& pixels,
        std::uint32_t& width,
        std::uint32_t& height,
        const avifImage* image)
    {
        // AVIF transform order: mirror (imir) first, then rotate (irot).
        std::vector<std::uint8_t> output;
        if ((image->transformFlags & AVIF_TRANSFORM_IMIR) != 0)
        {
            output.resize(pixels.size());
            // imir axis: 0 = top/bottom exchanged (vertical mirror),
            //            1 = left/right exchanged (horizontal mirror).
            const bool vertical_mirror = image->imir.axis == 0;
            for (std::uint32_t y = 0; y < height; ++y)
            {
                for (std::uint32_t x = 0; x < width; ++x)
                {
                    const auto source_x = vertical_mirror ? x : width - 1U - x;
                    const auto source_y = vertical_mirror ? height - 1U - y : y;
                    const auto* source = pixels.data() +
                        (static_cast<std::size_t>(source_y) * width + source_x) * 4U;
                    auto* destination = output.data() +
                        (static_cast<std::size_t>(y) * width + x) * 4U;
                    std::memcpy(destination, source, 4U);
                }
            }
            pixels = std::move(output);
        }
        const auto angle = static_cast<std::uint32_t>(image->irot.angle) & 3U;
        if ((image->transformFlags & AVIF_TRANSFORM_IROT) != 0 && angle != 0)
        {
            output.assign(pixels.size(), 0);
            if (angle == 2)
            {
                // 180 degrees.
                for (std::size_t index = 0; index < pixels.size(); index += 4U)
                {
                    const auto opposite = pixels.size() - 4U - index;
                    output[opposite] = pixels[index];
                    output[opposite + 1U] = pixels[index + 1U];
                    output[opposite + 2U] = pixels[index + 2U];
                    output[opposite + 3U] = pixels[index + 3U];
                }
                pixels = std::move(output);
                return;
            }
            const auto rotated_width = height;
            const auto rotated_height = width;
            for (std::uint32_t y = 0; y < height; ++y)
            {
                for (std::uint32_t x = 0; x < width; ++x)
                {
                    const auto* source = pixels.data() +
                        (static_cast<std::size_t>(y) * width + x) * 4U;
                    std::uint32_t destination_x{};
                    std::uint32_t destination_y{};
                    if (angle == 1)
                    {
                        // 90 degrees counter-clockwise.
                        destination_x = y;
                        destination_y = width - 1U - x;
                    }
                    else
                    {
                        // 270 degrees counter-clockwise.
                        destination_x = height - 1U - y;
                        destination_y = x;
                    }
                    auto* destination = output.data() +
                        (static_cast<std::size_t>(destination_y) * rotated_width +
                         destination_x) *
                            4U;
                    std::memcpy(destination, source, 4U);
                }
            }
            pixels = std::move(output);
            width = rotated_width;
            height = rotated_height;
        }
    }

    bool prepare_avif_preview(
        const std::filesystem::path& input,
        const std::filesystem::path& output,
        std::uint32_t maximum_dimension)
    {
        avifDecoder* decoder = avifDecoderCreate();
        if (decoder == nullptr)
        {
            return false;
        }
        const auto decoder_guard = std::unique_ptr<avifDecoder, decltype(&avifDecoderDestroy)>(
            decoder, avifDecoderDestroy);
        decoder->imageSizeLimit = maximum_decode_pixels;
        decoder->imageDimensionLimit = maximum_decode_dimension;
        decoder->maxThreads = std::min(
            maximum_decoder_threads,
            static_cast<int>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));

        avifImage* image = avifImageCreateEmpty();
        if (image == nullptr)
        {
            return false;
        }
        const auto image_guard = std::unique_ptr<avifImage, decltype(&avifImageDestroy)>(
            image, avifImageDestroy);

        // Read through the wide-character API so Unicode paths work, then
        // decode from memory; the buffer must outlive the decode below.
        HANDLE file = CreateFileW(
            input.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        const auto file_guard = std::unique_ptr<void, decltype(&CloseHandle)>(
            file, CloseHandle);
        LARGE_INTEGER file_size{};
        if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart <= 0 ||
            file_size.QuadPart > 256LL * 1024LL * 1024LL)
        {
            return false;
        }
        std::vector<std::uint8_t> file_data(static_cast<std::size_t>(file_size.QuadPart));
        DWORD bytes_read{};
        if (!ReadFile(
                file,
                file_data.data(),
                static_cast<DWORD>(file_data.size()),
                &bytes_read,
                nullptr) ||
            bytes_read != file_data.size())
        {
            return false;
        }

        const auto result = avifDecoderReadMemory(
            decoder, image, file_data.data(), file_data.size());
        if (result != AVIF_RESULT_OK)
        {
            return false;
        }

        const std::uint32_t original_width = image->width;
        const std::uint32_t original_height = image->height;
        if (original_width == 0 || original_height == 0 ||
            static_cast<std::uint64_t>(original_width) * original_height >
                maximum_decode_pixels)
        {
            return false;
        }

        const std::uint64_t larger_side =
            original_width > original_height ? original_width : original_height;
        if (larger_side > maximum_dimension)
        {
            const auto scaled_width = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(original_width) * maximum_dimension / larger_side);
            const auto scaled_height = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(original_height) * maximum_dimension / larger_side);
            if (scaled_width == 0 || scaled_height == 0)
            {
                return false;
            }
            avifDiagnostics diagnostics{};
            if (avifImageScale(image, scaled_width, scaled_height, &diagnostics) != AVIF_RESULT_OK)
            {
                return false;
            }
        }

        avifRGBImage rgb;
        avifRGBImageSetDefaults(&rgb, image);
        rgb.format = AVIF_RGB_FORMAT_BGRA;
        rgb.depth = 8;
        rgb.maxThreads = decoder->maxThreads;
        if (avifRGBImageAllocatePixels(&rgb) != AVIF_RESULT_OK)
        {
            return false;
        }
        if (avifImageYUVToRGB(image, &rgb) != AVIF_RESULT_OK)
        {
            avifRGBImageFreePixels(&rgb);
            return false;
        }

        std::uint32_t width = rgb.width;
        std::uint32_t height = rgb.height;
        const bool needs_transform =
            (image->transformFlags & (AVIF_TRANSFORM_IMIR | AVIF_TRANSFORM_IROT)) != 0;
        if (!needs_transform)
        {
            const bool written = write_png(
                output,
                rgb.pixels,
                width,
                height,
                rgb.rowBytes,
                true);
            avifRGBImageFreePixels(&rgb);
            return written;
        }

        std::vector<std::uint8_t> transformed(
            static_cast<std::size_t>(width) * height * 4U);
        for (std::uint32_t y = 0; y < height; ++y)
        {
            std::memcpy(
                transformed.data() + static_cast<std::size_t>(y) * width * 4U,
                rgb.pixels + static_cast<std::size_t>(y) * rgb.rowBytes,
                static_cast<std::size_t>(width) * 4U);
        }
        avifRGBImageFreePixels(&rgb);
        transform_pixels(transformed, width, height, image);
        return write_png(
            output,
            transformed.data(),
            width,
            height,
            width * 4U,
            true);
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
        return ERROR_GEN_FAILURE;
    }
    std::error_code output_error;
    const bool success =
        prepare_avif_preview(parsed.input, parsed.output, parsed.maximum_dimension) &&
        std::filesystem::is_regular_file(parsed.output, output_error);
    CoUninitialize();
    return success ? ERROR_SUCCESS : ERROR_GEN_FAILURE;
}

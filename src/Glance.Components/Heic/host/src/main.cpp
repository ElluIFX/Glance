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

#include "heif.h"

namespace
{
    using Microsoft::WRL::ComPtr;
    constexpr std::uint64_t maximum_decode_pixels = 100'000'000ULL;
    constexpr std::uint64_t maximum_codec_memory = 768ULL * 1024ULL * 1024ULL;

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
        WICPixelFormatGUID pixel_format = has_alpha
            ? GUID_WICPixelFormat32bppRGBA
            : GUID_WICPixelFormat24bppRGB;
        if (FAILED(frame->SetPixelFormat(&pixel_format)))
        {
            return false;
        }
        ComPtr<IWICBitmap> bitmap;
        if (FAILED(factory->CreateBitmapFromMemory(
                width,
                height,
                pixel_format,
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

    bool prepare_heic_preview(
        const std::filesystem::path& input,
        const std::filesystem::path& output,
        std::uint32_t maximum_dimension)
    {
        heif_context* context = heif_context_alloc();
        if (context == nullptr)
        {
            return false;
        }
        const auto context_guard = std::unique_ptr<heif_context, decltype(&heif_context_free)>(
            context, heif_context_free);
        if (auto* limits = heif_context_get_security_limits(context); limits != nullptr)
        {
            limits->max_image_size_pixels = maximum_decode_pixels;
            limits->max_memory_block_size = maximum_codec_memory;
            limits->max_total_memory = maximum_codec_memory;
        }

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

        heif_error error = heif_context_read_from_memory_without_copy(
            context,
            file_data.data(),
            file_data.size(),
            nullptr);
        if (error.code != heif_error_Ok)
        {
            return false;
        }

        heif_image_handle* handle{};
        error = heif_context_get_primary_image_handle(context, &handle);
        if (error.code != heif_error_Ok)
        {
            return false;
        }
        const auto handle_guard = std::unique_ptr<heif_image_handle, decltype(&heif_image_handle_release)>(
            handle, heif_image_handle_release);
        const auto handle_width = heif_image_handle_get_width(handle);
        const auto handle_height = heif_image_handle_get_height(handle);
        if (handle_width <= 0 || handle_height <= 0 ||
            static_cast<std::uint64_t>(handle_width) * handle_height > maximum_decode_pixels)
        {
            return false;
        }

        const bool has_alpha = heif_image_handle_has_alpha_channel(handle) != 0;
        heif_image* decoded{};
        error = heif_decode_image(
            handle,
            &decoded,
            heif_colorspace_RGB,
            has_alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB,
            nullptr);
        if (error.code != heif_error_Ok)
        {
            return false;
        }
        const auto image_guard = std::unique_ptr<heif_image, decltype(&heif_image_release)>(
            decoded, heif_image_release);

        const std::uint32_t original_width = heif_image_get_width(decoded, heif_channel_interleaved);
        const std::uint32_t original_height = heif_image_get_height(decoded, heif_channel_interleaved);
        if (original_width == 0 || original_height == 0)
        {
            return false;
        }

        const std::uint64_t larger_side =
            original_width > original_height ? original_width : original_height;
        if (larger_side <= maximum_dimension)
        {
            int stride{};
            const auto* pixels = heif_image_get_plane_readonly(
                decoded,
                heif_channel_interleaved,
                &stride);
            if (pixels == nullptr || stride <= 0)
            {
                return false;
            }
            return write_png(
                output,
                pixels,
                original_width,
                original_height,
                static_cast<std::uint32_t>(stride),
                has_alpha);
        }

        heif_image* scaled{};
        error = heif_image_scale_image(
            decoded,
            &scaled,
            static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(original_width) * maximum_dimension / larger_side),
            static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(original_height) * maximum_dimension / larger_side),
            nullptr);
        if (error.code != heif_error_Ok)
        {
            return false;
        }
        const auto scaled_guard = std::unique_ptr<heif_image, decltype(&heif_image_release)>(
            scaled, heif_image_release);
        int stride{};
        const auto* pixels = heif_image_get_plane_readonly(
            scaled,
            heif_channel_interleaved,
            &stride);
        if (pixels == nullptr || stride <= 0)
        {
            return false;
        }
        return write_png(
            output,
            pixels,
            heif_image_get_width(scaled, heif_channel_interleaved),
            heif_image_get_height(scaled, heif_channel_interleaved),
            static_cast<std::uint32_t>(stride),
            has_alpha);
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
    const bool success =
        prepare_heic_preview(parsed.input, parsed.output, parsed.maximum_dimension) &&
        std::filesystem::is_regular_file(parsed.output, output_error);
    CoUninitialize();
    return success ? ERROR_SUCCESS : ERROR_GEN_FAILURE;
}

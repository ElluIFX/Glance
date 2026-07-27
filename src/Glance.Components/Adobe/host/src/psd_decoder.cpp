#include "../include/psd_decoder.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <vector>

#define STBI_ONLY_PSD
#define STBI_MAX_DIMENSIONS 65536
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace
{
    using Microsoft::WRL::ComPtr;

    bool read_exact(
        std::istream& input,
        void* destination,
        std::size_t size)
    {
        return static_cast<bool>(input.read(
            static_cast<char*>(destination),
            static_cast<std::streamsize>(size)));
    }

    bool read_be16(std::istream& input, std::uint16_t& value)
    {
        std::array<unsigned char, 2> bytes{};
        if (!read_exact(input, bytes.data(), bytes.size()))
        {
            return false;
        }
        value = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(bytes[0]) << 8) |
            bytes[1]);
        return true;
    }

    bool read_be32(std::istream& input, std::uint32_t& value)
    {
        std::array<unsigned char, 4> bytes{};
        if (!read_exact(input, bytes.data(), bytes.size()))
        {
            return false;
        }
        value =
            (static_cast<std::uint32_t>(bytes[0]) << 24) |
            (static_cast<std::uint32_t>(bytes[1]) << 16) |
            (static_cast<std::uint32_t>(bytes[2]) << 8) |
            bytes[3];
        return true;
    }

    bool skip_section(std::istream& input)
    {
        std::uint32_t size{};
        if (!read_be32(input, size))
        {
            return false;
        }
        input.seekg(static_cast<std::streamoff>(size), std::ios::cur);
        return static_cast<bool>(input);
    }

    bool decode_packbits_row(
        const std::vector<unsigned char>& encoded,
        std::vector<unsigned char>& decoded)
    {
        std::size_t source{};
        std::size_t destination{};
        while (source < encoded.size() && destination < decoded.size())
        {
            const auto control =
                static_cast<std::int8_t>(encoded[source++]);
            if (control >= 0)
            {
                const auto count = static_cast<std::size_t>(control) + 1;
                if (source + count > encoded.size() ||
                    destination + count > decoded.size())
                {
                    return false;
                }
                std::copy_n(
                    encoded.data() + source,
                    count,
                    decoded.data() + destination);
                source += count;
                destination += count;
            }
            else if (control != std::numeric_limits<std::int8_t>::min())
            {
                const auto count =
                    static_cast<std::size_t>(1 - control);
                if (source >= encoded.size() ||
                    destination + count > decoded.size())
                {
                    return false;
                }
                std::fill_n(
                    decoded.data() + destination,
                    count,
                    encoded[source++]);
                destination += count;
            }
        }
        return destination == decoded.size();
    }

    void store_cmyk_row(
        std::vector<unsigned char>& pixels,
        std::vector<unsigned char>& black,
        const std::vector<unsigned char>& row,
        std::uint32_t width,
        std::uint32_t row_index,
        std::uint16_t depth,
        std::uint16_t channel)
    {
        if (channel > 4)
        {
            return;
        }
        const auto bytes_per_sample = depth / 8;
        const auto row_offset =
            static_cast<std::size_t>(row_index) * width;
        for (std::uint32_t column = 0; column < width; ++column)
        {
            const auto value =
                row[static_cast<std::size_t>(column) * bytes_per_sample];
            const auto pixel_index = row_offset + column;
            if (channel < 3)
            {
                pixels[pixel_index * 4 + channel] = value;
            }
            else if (channel == 3)
            {
                black[pixel_index] = value;
            }
            else
            {
                pixels[pixel_index * 4 + 3] = value;
            }
        }
    }

    bool decode_cmyk_psd(
        const std::filesystem::path& input_path,
        std::vector<unsigned char>& pixels,
        std::uint32_t& width,
        std::uint32_t& height)
    {
        std::ifstream input(input_path, std::ios::binary);
        std::array<char, 4> signature{};
        std::uint16_t version{};
        std::array<char, 6> reserved{};
        std::uint16_t channels{};
        std::uint16_t depth{};
        std::uint16_t color_mode{};
        if (!read_exact(input, signature.data(), signature.size()) ||
            signature != std::array<char, 4>{ '8', 'B', 'P', 'S' } ||
            !read_be16(input, version) ||
            version != 1 ||
            !read_exact(input, reserved.data(), reserved.size()) ||
            !read_be16(input, channels) ||
            !read_be32(input, height) ||
            !read_be32(input, width) ||
            !read_be16(input, depth) ||
            !read_be16(input, color_mode) ||
            color_mode != 4 ||
            channels < 4 ||
            channels > 16 ||
            (depth != 8 && depth != 16) ||
            width == 0 ||
            height == 0 ||
            width > STBI_MAX_DIMENSIONS ||
            height > STBI_MAX_DIMENSIONS ||
            !skip_section(input) ||
            !skip_section(input) ||
            !skip_section(input))
        {
            return false;
        }

        std::uint16_t compression{};
        if (!read_be16(input, compression) || compression > 1)
        {
            return false;
        }
        const auto pixel_count =
            static_cast<std::uint64_t>(width) * height;
        if (pixel_count >
            std::numeric_limits<std::size_t>::max() / 4)
        {
            return false;
        }
        pixels.assign(
            static_cast<std::size_t>(pixel_count) * 4,
            0);
        std::vector<unsigned char> black(
            static_cast<std::size_t>(pixel_count));
        for (std::size_t index = 3; index < pixels.size(); index += 4)
        {
            pixels[index] = 255;
        }

        const auto row_size =
            static_cast<std::size_t>(width) * (depth / 8);
        std::vector<std::uint16_t> compressed_sizes;
        if (compression == 1)
        {
            const auto row_count =
                static_cast<std::uint64_t>(channels) * height;
            if (row_count >
                std::numeric_limits<std::size_t>::max())
            {
                return false;
            }
            compressed_sizes.resize(
                static_cast<std::size_t>(row_count));
            for (auto& size : compressed_sizes)
            {
                if (!read_be16(input, size))
                {
                    return false;
                }
            }
        }

        std::vector<unsigned char> row(row_size);
        std::vector<unsigned char> encoded;
        for (std::uint16_t channel = 0; channel < channels; ++channel)
        {
            for (std::uint32_t row_index = 0;
                 row_index < height;
                 ++row_index)
            {
                if (compression == 0)
                {
                    if (!read_exact(input, row.data(), row.size()))
                    {
                        return false;
                    }
                }
                else
                {
                    const auto encoded_size = compressed_sizes[
                        static_cast<std::size_t>(channel) * height +
                        row_index];
                    encoded.resize(encoded_size);
                    if (!read_exact(
                            input,
                            encoded.data(),
                            encoded.size()) ||
                        !decode_packbits_row(encoded, row))
                    {
                        return false;
                    }
                }
                store_cmyk_row(
                    pixels,
                    black,
                    row,
                    width,
                    row_index,
                    depth,
                    channel);
            }
        }

        for (std::size_t index = 0;
             index < static_cast<std::size_t>(pixel_count);
             ++index)
        {
            auto* pixel = pixels.data() + index * 4;
            const auto key = black[index];
            pixel[0] = static_cast<unsigned char>(
                (static_cast<std::uint32_t>(pixel[0]) * key + 127) /
                255);
            pixel[1] = static_cast<unsigned char>(
                (static_cast<std::uint32_t>(pixel[1]) * key + 127) /
                255);
            pixel[2] = static_cast<unsigned char>(
                (static_cast<std::uint32_t>(pixel[2]) * key + 127) /
                255);
        }
        return true;
    }

    std::vector<unsigned char> read_file(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        const auto length = input.tellg();
        if (length <= 0 ||
            length > static_cast<std::streamoff>(std::numeric_limits<int>::max()))
        {
            return {};
        }
        std::vector<unsigned char> bytes(static_cast<std::size_t>(length));
        input.seekg(0);
        if (!input.read(
                reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size())))
        {
            return {};
        }
        return bytes;
    }

    bool write_png(
        const std::filesystem::path& output,
        const unsigned char* pixels,
        std::uint32_t width,
        std::uint32_t height,
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

        const auto longest = std::max(width, height);
        const double scale = longest > maximum_dimension
            ? static_cast<double>(maximum_dimension) / longest
            : 1.0;
        const auto output_width = std::max(
            1U,
            static_cast<std::uint32_t>(std::lround(width * scale)));
        const auto output_height = std::max(
            1U,
            static_cast<std::uint32_t>(std::lround(height * scale)));
        const std::uint64_t stride = static_cast<std::uint64_t>(width) * 4U;
        const std::uint64_t byte_count = stride * height;
        if (stride > std::numeric_limits<UINT>::max() ||
            byte_count > std::numeric_limits<UINT>::max())
        {
            return false;
        }

        ComPtr<IWICBitmap> source;
        if (FAILED(factory->CreateBitmapFromMemory(
                width,
                height,
                GUID_WICPixelFormat32bppRGBA,
                static_cast<UINT>(stride),
                static_cast<UINT>(byte_count),
                const_cast<BYTE*>(pixels),
                &source)))
        {
            return false;
        }

        ComPtr<IWICBitmapSource> encoded_source = source;
        ComPtr<IWICBitmapScaler> scaler;
        if (output_width != width || output_height != height)
        {
            if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
                FAILED(scaler->Initialize(
                    source.Get(),
                    output_width,
                    output_height,
                    WICBitmapInterpolationModeFant)))
            {
                return false;
            }
            encoded_source = scaler;
        }

        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        if (FAILED(factory->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromFilename(
                output.c_str(),
                GENERIC_WRITE)) ||
            FAILED(factory->CreateEncoder(
                GUID_ContainerFormatPng,
                nullptr,
                &encoder)) ||
            FAILED(encoder->Initialize(
                stream.Get(),
                WICBitmapEncoderNoCache)) ||
            FAILED(encoder->CreateNewFrame(&frame, nullptr)) ||
            FAILED(frame->Initialize(nullptr)) ||
            FAILED(frame->SetSize(output_width, output_height)))
        {
            return false;
        }

        WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
        if (FAILED(frame->SetPixelFormat(&format)) ||
            FAILED(frame->WriteSource(encoded_source.Get(), nullptr)) ||
            FAILED(frame->Commit()) ||
            FAILED(encoder->Commit()))
        {
            return false;
        }
        return true;
    }
}

namespace glance::components::adobe
{
    bool prepare_psd_preview(
        const std::filesystem::path& input,
        const std::filesystem::path& output,
        std::uint32_t maximum_dimension) noexcept
    {
        try
        {
            std::vector<unsigned char> cmyk_pixels;
            std::uint32_t cmyk_width{};
            std::uint32_t cmyk_height{};
            if (decode_cmyk_psd(
                    input,
                    cmyk_pixels,
                    cmyk_width,
                    cmyk_height))
            {
                return write_png(
                    output,
                    cmyk_pixels.data(),
                    cmyk_width,
                    cmyk_height,
                    maximum_dimension);
            }

            const auto bytes = read_file(input);
            if (bytes.empty())
            {
                return false;
            }

            int width{};
            int height{};
            int channels{};
            unsigned char* pixels = stbi_load_from_memory(
                bytes.data(),
                static_cast<int>(bytes.size()),
                &width,
                &height,
                &channels,
                STBI_rgb_alpha);
            if (pixels == nullptr || width <= 0 || height <= 0)
            {
                stbi_image_free(pixels);
                return false;
            }

            const bool success = write_png(
                output,
                pixels,
                static_cast<std::uint32_t>(width),
                static_cast<std::uint32_t>(height),
                maximum_dimension);
            stbi_image_free(pixels);
            return success;
        }
        catch (...)
        {
            return false;
        }
    }
}

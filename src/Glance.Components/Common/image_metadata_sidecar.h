#pragma once

#include "glance/contracts/component_api.h"

#include <filesystem>
#include <fstream>
#include <span>
#include <type_traits>
#include <vector>

namespace glance::components
{
    namespace detail
    {
        inline constexpr std::uint32_t image_metadata_sidecar_magic = 0x58454D47;
        inline constexpr std::uint32_t image_metadata_sidecar_version = 1;
        inline constexpr std::uint32_t maximum_image_metadata_entries = 128;

        struct ImageMetadataSidecarHeader
        {
            std::uint32_t magic{ image_metadata_sidecar_magic };
            std::uint32_t version{ image_metadata_sidecar_version };
            std::uint32_t entry_size{
                sizeof(glance::contracts::components::ImageMetadataEntry) };
            std::uint32_t count{};
        };
    }

    inline bool write_image_metadata_sidecar(
        const std::filesystem::path& path,
        std::span<const glance::contracts::components::ImageMetadataEntry> entries) noexcept
    {
        using glance::contracts::components::ImageMetadataEntry;
        static_assert(std::is_trivially_copyable_v<ImageMetadataEntry>);
        try
        {
            if (entries.size() > detail::maximum_image_metadata_entries)
            {
                return false;
            }
            const detail::ImageMetadataSidecarHeader header{
                .count = static_cast<std::uint32_t>(entries.size()) };
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                return false;
            }
            output.write(
                reinterpret_cast<const char*>(&header),
                sizeof(header));
            if (!entries.empty())
            {
                output.write(
                    reinterpret_cast<const char*>(entries.data()),
                    static_cast<std::streamsize>(entries.size_bytes()));
            }
            return output.good();
        }
        catch (...)
        {
            return false;
        }
    }

    inline std::vector<glance::contracts::components::ImageMetadataEntry>
        read_image_metadata_sidecar(const std::filesystem::path& path) noexcept
    {
        using glance::contracts::components::ImageMetadataEntry;
        try
        {
            std::ifstream input(path, std::ios::binary);
            detail::ImageMetadataSidecarHeader header;
            if (!input.read(reinterpret_cast<char*>(&header), sizeof(header)) ||
                header.magic != detail::image_metadata_sidecar_magic ||
                header.version != detail::image_metadata_sidecar_version ||
                header.entry_size != sizeof(ImageMetadataEntry) ||
                header.count > detail::maximum_image_metadata_entries)
            {
                return {};
            }
            std::vector<ImageMetadataEntry> entries(header.count);
            if (!entries.empty() &&
                !input.read(
                    reinterpret_cast<char*>(entries.data()),
                    static_cast<std::streamsize>(entries.size() * sizeof(ImageMetadataEntry))))
            {
                return {};
            }
            if (input.peek() != std::ifstream::traits_type::eof())
            {
                return {};
            }
            return entries;
        }
        catch (...)
        {
            return {};
        }
    }
}

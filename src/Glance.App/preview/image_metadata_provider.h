#pragma once

#include "glance/contracts/component_api.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace glance::app
{
    enum class ImageMetadataSection : std::uint32_t
    {
        capture,
        camera,
        image,
        location,
        details,
    };

    struct ImageMetadataEntry
    {
        ImageMetadataSection section{};
        std::wstring canonical_name;
        std::wstring name;
        std::wstring value;
    };

    struct ImageMetadata
    {
        std::vector<ImageMetadataEntry> entries;
        std::wstring taken_time;
    };

    [[nodiscard]] ImageMetadata load_image_metadata(const std::wstring& path);
    void merge_component_image_metadata(
        ImageMetadata& metadata,
        std::span<const glance::contracts::components::ImageMetadataEntry> entries);
    [[nodiscard]] std::uint32_t load_image_bit_depth(const std::wstring& path) noexcept;
}

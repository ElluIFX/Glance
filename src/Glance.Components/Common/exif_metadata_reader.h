#pragma once

#include "glance/contracts/component_api.h"

#include <cstdint>
#include <span>
#include <vector>

namespace glance::components
{
    [[nodiscard]] std::vector<glance::contracts::components::ImageMetadataEntry>
        read_exif_metadata(std::span<const std::uint8_t> data) noexcept;
}

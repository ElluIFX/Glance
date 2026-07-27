#pragma once

#include <cstdint>
#include <filesystem>

namespace glance::components::adobe
{
    [[nodiscard]] bool prepare_psd_preview(
        const std::filesystem::path& input,
        const std::filesystem::path& output,
        std::uint32_t maximum_dimension) noexcept;
}

#pragma once

#include <cstdint>
#include <string>

namespace glance::app
{
    [[nodiscard]] std::wstring load_image_metadata(const std::wstring& path);
    [[nodiscard]] std::uint32_t load_image_bit_depth(const std::wstring& path) noexcept;
}

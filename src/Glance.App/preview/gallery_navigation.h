#pragma once

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <string>

namespace glance::app
{
    [[nodiscard]] inline std::wstring normalize_gallery_extension(
        std::wstring extension) noexcept
    {
        std::transform(
            extension.begin(),
            extension.end(),
            extension.begin(),
            [](wchar_t value) { return std::towlower(value); });
        return extension;
    }

    [[nodiscard]] inline std::uint32_t gallery_target_index(
        std::uint32_t current_index,
        int steps,
        std::uint32_t total_count,
        bool loop) noexcept
    {
        if (total_count == 0)
        {
            return current_index;
        }
        const auto total = static_cast<std::int64_t>(total_count);
        if (!loop)
        {
            return static_cast<std::uint32_t>(std::clamp(
                static_cast<std::int64_t>(current_index) + steps,
                std::int64_t{},
                total - 1));
        }
        auto target = (static_cast<std::int64_t>(current_index % total_count) +
            static_cast<std::int64_t>(steps) % total) % total;
        if (target < 0)
        {
            target += total;
        }
        return static_cast<std::uint32_t>(target);
    }
}

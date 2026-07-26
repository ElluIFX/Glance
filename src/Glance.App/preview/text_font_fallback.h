#pragma once

#include <array>
#include <cwctype>
#include <span>
#include <string>
#include <string_view>

namespace glance::app
{
    inline constexpr std::array<std::wstring_view, 5> preferred_text_font_families{
        L"Cascadia Mono",
        L"Consolas",
        L"Courier New",
        L"Lucida Console",
        L"NSimSun"
    };

    [[nodiscard]] inline bool font_family_names_equal(
        std::wstring_view left,
        std::wstring_view right) noexcept
    {
        if (left.size() != right.size())
        {
            return false;
        }
        for (std::size_t index = 0; index < left.size(); ++index)
        {
            if (std::towlower(left[index]) != std::towlower(right[index]))
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] inline std::wstring select_default_text_font_family(
        std::span<const std::wstring> installed_font_families)
    {
        for (const auto preferred : preferred_text_font_families)
        {
            for (const auto& installed : installed_font_families)
            {
                if (font_family_names_equal(preferred, installed))
                {
                    return installed;
                }
            }
        }
        return std::wstring(preferred_text_font_families.front());
    }
}

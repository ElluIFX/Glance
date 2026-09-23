#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace glance::app
{
    enum class SyntaxThemePreference : std::uint32_t
    {
        glance,
        visual_studio,
        monokai,
        github,
        dracula,
        solarized,
        nord,
        one_dark,
        gruvbox,
        tomorrow_night,
        catppuccin,
        tokyo_night,
        rose_pine,
        everforest,
        ayu,
        horizon,
        papercolor,
        material,
    };

    struct TextPreferences
    {
        std::wstring font_family{ L"Cascadia Mono" };
        double font_size{ 9.0 };
        SyntaxThemePreference syntax_theme{ SyntaxThemePreference::glance };
        bool word_wrap{ true };
        bool syntax_highlighting{ true };
        bool line_numbers{ true };
    };

    [[nodiscard]] TextPreferences load_text_preferences();
    [[nodiscard]] std::vector<std::wstring> system_font_families();
    void save_text_preferences(const TextPreferences& preferences) noexcept;
}

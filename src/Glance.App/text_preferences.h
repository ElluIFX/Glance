#pragma once

#include <string>
#include <vector>

namespace glance::app
{
    struct TextPreferences
    {
        std::wstring font_family{ L"Cascadia Mono" };
        double font_size{ 13.0 };
        bool word_wrap{};
        bool syntax_highlighting{ true };
        bool line_numbers{ true };
    };

    [[nodiscard]] TextPreferences load_text_preferences();
    [[nodiscard]] std::vector<std::wstring> system_font_families();
    void save_text_preferences(const TextPreferences& preferences) noexcept;
}

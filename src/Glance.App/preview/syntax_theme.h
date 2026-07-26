#pragma once

#include "text_preferences.h"

#include <cstdint>

namespace glance::app
{
    struct SyntaxThemePalette
    {
        std::uint32_t background{};
        std::uint32_t foreground{};
        std::uint32_t selection{};
        std::uint32_t line_number{};
        std::uint32_t line_number_background{};
        std::uint32_t comment{};
        std::uint32_t keyword{};
        std::uint32_t type{};
        std::uint32_t function{};
        std::uint32_t string{};
        std::uint32_t number{};
        std::uint32_t constant{};
        std::uint32_t preprocessor{};
        std::uint32_t tag{};
        std::uint32_t attribute{};
        std::uint32_t escape{};
        std::uint32_t error{};
    };

    [[nodiscard]] const SyntaxThemePalette& syntax_theme_palette(
        SyntaxThemePreference theme,
        bool dark) noexcept;
}

#pragma once

#include <winrt/Microsoft.UI.Xaml.h>

#include <cstdint>
#include <string>

namespace glance::app
{
    enum class ThemePreference : std::uint32_t
    {
        system,
        light,
        dark,
    };

    enum class AccentPreference : std::uint32_t
    {
        system,
        blue,
        teal,
        green,
        orange,
        red,
        pink,
        purple,
    };

    struct AppearancePreferences
    {
        ThemePreference theme{ ThemePreference::system };
        AccentPreference accent{ AccentPreference::system };
        bool acrylic_enabled{};
        std::uint32_t acrylic_opacity_percent{ 100 };
        std::wstring language;
    };

    [[nodiscard]] bool acrylic_material_supported() noexcept;
    [[nodiscard]] AppearancePreferences load_appearance_preferences() noexcept;
    void save_appearance_preferences(const AppearancePreferences& preferences) noexcept;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::ElementTheme element_theme(ThemePreference preference) noexcept;
    void apply_accent_resources(const AppearancePreferences& preferences);
}

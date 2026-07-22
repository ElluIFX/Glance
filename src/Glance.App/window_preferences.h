#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace glance::app
{
    struct WindowPreferences
    {
        std::uint32_t default_width{ 720 };
        std::uint32_t default_height{ 520 };
        bool remember_size{ true };
        bool auto_fit_media{ true };
        bool dynamic_auto_fit{ true };
        std::uint32_t adaptive_minimum_percent{ 40 };
        std::uint32_t adaptive_maximum_percent{ 75 };
        std::wstring auto_fit_ignored_extensions;
        bool remember_position{};
        std::uint32_t opacity_percent{ 100 };
    };

    [[nodiscard]] WindowPreferences load_window_preferences() noexcept;
    void save_window_preferences(const WindowPreferences& preferences) noexcept;
    [[nodiscard]] bool auto_fit_ignores_path(
        const WindowPreferences& preferences,
        std::wstring_view path) noexcept;
}

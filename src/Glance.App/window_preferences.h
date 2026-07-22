#pragma once

#include <cstdint>

namespace glance::app
{
    struct WindowPreferences
    {
        std::uint32_t default_width{ 720 };
        std::uint32_t default_height{ 520 };
        bool remember_size{ true };
        bool auto_fit_media{ true };
        bool remember_position{};
        std::uint32_t opacity_percent{ 100 };
    };

    [[nodiscard]] WindowPreferences load_window_preferences() noexcept;
    void save_window_preferences(const WindowPreferences& preferences) noexcept;
}

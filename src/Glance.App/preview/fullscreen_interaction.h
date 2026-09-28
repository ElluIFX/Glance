#pragma once

namespace glance::app
{
    [[nodiscard]] constexpr unsigned int fullscreen_double_click_interval(unsigned int system_interval) noexcept
    {
        return system_interval > 1 ? system_interval / 2 : 1;
    }

    [[nodiscard]] constexpr bool can_toggle_preview_fullscreen(
        bool visible,
        bool enabled,
        bool password_prompt_active,
        bool toggle_pending) noexcept
    {
        return visible && enabled && !password_prompt_active && !toggle_pending;
    }

    [[nodiscard]] constexpr bool should_handle_xaml_fullscreen_double_tap(
        bool handled,
        bool web_preview_visible,
        bool interactive_source) noexcept
    {
        return !handled && !web_preview_visible && !interactive_source;
    }
}

#pragma once

namespace glance::app
{
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

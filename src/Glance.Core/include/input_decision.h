#pragma once

#include <windows.h>

namespace glance::core
{
    [[nodiscard]] constexpr bool should_capture_key(
        DWORD virtual_key,
        bool connected,
        bool preview_active,
        bool eligible_selection,
        bool modified) noexcept
    {
        if (!connected || modified)
        {
            return false;
        }
        return (virtual_key == VK_SPACE && (preview_active || eligible_selection)) ||
               (virtual_key == VK_ESCAPE && preview_active);
    }
}

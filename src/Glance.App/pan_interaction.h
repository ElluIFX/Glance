#pragma once

#include <algorithm>

namespace glance::app
{
    struct PanPoint
    {
        double x{};
        double y{};
    };

    struct PanOffsets
    {
        double horizontal{};
        double vertical{};
    };

    [[nodiscard]] constexpr bool zoom_allows_pan(float zoom_factor) noexcept
    {
        return zoom_factor > 1.001F;
    }

    [[nodiscard]] constexpr PanOffsets calculate_pan_offsets(
        PanOffsets initial_offsets,
        PanPoint initial_pointer,
        PanPoint current_pointer) noexcept
    {
        return {
            std::max(0.0, initial_offsets.horizontal + initial_pointer.x - current_pointer.x),
            std::max(0.0, initial_offsets.vertical + initial_pointer.y - current_pointer.y)
        };
    }
}

#pragma once

#include "preview_provider.h"

#include <windows.h>

#include <optional>

namespace glance::app
{
    [[nodiscard]] std::optional<SIZE> load_window_size(PreviewKind kind, bool media_is_audio = false);
    void save_window_size(PreviewKind kind, SIZE size, bool media_is_audio = false) noexcept;
    [[nodiscard]] bool clear_window_sizes() noexcept;
    [[nodiscard]] std::optional<POINT> load_window_center_offset(
        PreviewKind kind,
        bool media_is_audio = false);
    void save_window_center_offset(
        PreviewKind kind,
        POINT offset,
        bool media_is_audio = false) noexcept;
    [[nodiscard]] bool clear_window_positions() noexcept;
}

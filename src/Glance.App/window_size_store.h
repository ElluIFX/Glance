#pragma once

#include "preview_provider.h"

#include <windows.h>

#include <optional>

namespace glance::app
{
    [[nodiscard]] std::optional<SIZE> load_window_size(PreviewKind kind);
    void save_window_size(PreviewKind kind, SIZE size) noexcept;
    [[nodiscard]] bool clear_window_sizes() noexcept;
}

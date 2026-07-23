#pragma once

#include <cstdint>

namespace glance::contracts
{
    enum class PreviewWindowState : std::uint8_t
    {
        hidden,
        active_following,
        active_topmost,
        active_pinned,
        detached_pinned_topmost,
        detached_unpinned,
        closed,
        active_interactive,
    };

    struct InteractionSnapshot
    {
        std::uintptr_t foreground_window{};
        std::uint32_t foreground_process_id{};
        std::uint32_t selection_count{};
        std::uint64_t selection_generation{};
        std::uint64_t timestamp_ms{};
        PreviewWindowState preview_state{ PreviewWindowState::hidden };
        bool focus_is_file_list{};
        bool ui_connected{};
    };
}

#pragma once

#include "preview_provider.h"
#include <windows.h>
#include <optional>
#include <string>
#include <string_view>

namespace glance::app
{
    // A preview owns its memory independently of the surface used to render it.
    struct WindowPlacementIdentity
    {
        std::wstring key;

        [[nodiscard]] static WindowPlacementIdentity component(std::wstring_view id)
        {
            return {id.empty() ? std::wstring{} : L"Component." + std::wstring(id)};
        }
        [[nodiscard]] static WindowPlacementIdentity builtin(PreviewKind kind, bool audio = false)
        {
            switch (kind)
            {
            case PreviewKind::text: return {L"Text"};
            case PreviewKind::markdown: return {L"Markdown"};
            case PreviewKind::web: return {L"Web"};
            case PreviewKind::image: return {L"Image"};
            case PreviewKind::media: return {audio ? L"Audio" : L"Video"};
            case PreviewKind::archive: return {L"Archive"};
            case PreviewKind::generic: return {L"Generic"};
            default: return {}; // Unresolved components have no persistence identity.
            }
        }
        [[nodiscard]] bool operator==(const WindowPlacementIdentity&) const = default;
    };

    struct WindowPlacementMemory
    {
        std::optional<SIZE> size;
        std::optional<POINT> center_offset;
    };

    [[nodiscard]] WindowPlacementMemory load_window_placement(const WindowPlacementIdentity& identity);
    void save_window_placement(const WindowPlacementIdentity& identity, const WindowPlacementMemory& memory) noexcept;
    [[nodiscard]] bool clear_window_sizes() noexcept;
    [[nodiscard]] bool clear_window_positions() noexcept;
}

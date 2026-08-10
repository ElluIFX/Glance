#pragma once

#include <windows.h>
#include <wrl/client.h>

#include "dialog_hook_client.h"
#include "external_host_provider.h"
#include "glance/contracts/file_descriptor.h"

#include <string>
#include <string_view>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

struct IUIAutomation2;

namespace glance::core
{
    enum class GalleryOperation
    {
        open,
        page,
        select,
        close,
    };

    struct GalleryCommand
    {
        GalleryOperation operation{};
        std::uint64_t window_id{};
        std::uint64_t request_id{};
        std::uint64_t session_id{};
        std::uintptr_t source_window{};
        std::uint32_t page_start{};
        std::uint32_t page_count{};
        std::uint32_t target_index{};
        std::wstring current_path;
        std::vector<std::wstring> extensions;
    };

    struct GalleryResponse
    {
        GalleryOperation operation{};
        std::uint64_t window_id{};
        std::uint64_t request_id{};
        std::uint64_t session_id{};
        bool success{};
        std::wstring error;
        std::uint32_t total_count{};
        std::uint32_t current_index{};
        std::uint32_t page_start{};
        std::vector<glance::contracts::FileDescriptor> items;
    };

    class ExplorerSelectionService
    {
    public:
        ExplorerSelectionService();
        ~ExplorerSelectionService();

        [[nodiscard]] glance::contracts::SelectionSnapshot query_foreground();
        [[nodiscard]] GalleryResponse handle_gallery_command(
            const GalleryCommand& command,
            const std::function<bool()>& canceled,
            const std::function<void()>& report_progress);
        [[nodiscard]] bool consume_gallery_selection_sync(
            const glance::contracts::SelectionSnapshot& snapshot);

    private:
        struct GallerySessionItem
        {
            std::wstring path;
            std::uint32_t view_index{};
        };

        struct GallerySession
        {
            std::uint64_t id{};
            std::uintptr_t source_window{};
            std::uintptr_t view_window{};
            std::uint32_t current_index{};
            std::wstring folder_path;
            std::vector<GallerySessionItem> items;
        };

        [[nodiscard]] bool is_text_input_focused() const;

        ExternalHostProviderRegistry external_hosts_;
        mutable Microsoft::WRL::ComPtr<IUIAutomation2> automation_;
        DialogHookClient dialog_hook_;
        HWND dialog_cache_window_{};
        ULONGLONG dialog_cache_timestamp_{};
        std::wstring dialog_cache_path_;
        std::unordered_map<std::uint64_t, GallerySession> gallery_sessions_;
        std::uint64_t next_gallery_session_id_{};
        std::uintptr_t synchronized_source_window_{};
        std::wstring synchronized_path_;
        ULONGLONG synchronized_selection_expires_{};
    };
}

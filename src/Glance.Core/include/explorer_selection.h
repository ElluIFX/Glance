#pragma once

#include <windows.h>
#include <wrl/client.h>

#include "dialog_hook_client.h"
#include "glance/contracts/file_descriptor.h"

#include <string>

struct IUIAutomation2;

namespace glance::core
{
    class ExplorerSelectionService
    {
    public:
        ExplorerSelectionService();
        ~ExplorerSelectionService();

        [[nodiscard]] glance::contracts::SelectionSnapshot query_foreground();

    private:
        [[nodiscard]] static bool is_explorer_window(HWND window, DWORD& process_id);
        [[nodiscard]] bool is_text_input_focused() const;

        mutable Microsoft::WRL::ComPtr<IUIAutomation2> automation_;
        DialogHookClient dialog_hook_;
        HWND dialog_cache_window_{};
        ULONGLONG dialog_cache_timestamp_{};
        std::wstring dialog_cache_path_;
    };
}

#pragma once

#include <windows.h>
#include <wrl/client.h>

#include "glance/contracts/file_descriptor.h"

struct IUIAutomation2;

namespace glance::core
{
    class ExplorerSelectionService
    {
    public:
        ExplorerSelectionService();
        ~ExplorerSelectionService();

        [[nodiscard]] glance::contracts::SelectionSnapshot query_foreground() const;

    private:
        [[nodiscard]] static bool is_explorer_window(HWND window, DWORD& process_id);
        [[nodiscard]] bool is_text_input_focused(const GUITHREADINFO& thread_info) const;

        mutable Microsoft::WRL::ComPtr<IUIAutomation2> automation_;
    };
}

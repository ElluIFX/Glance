#pragma once

#include <windows.h>

#include "glance/contracts/file_descriptor.h"

namespace glance::core
{
    class ExplorerSelectionService
    {
    public:
        [[nodiscard]] glance::contracts::SelectionSnapshot query_foreground() const;

    private:
        [[nodiscard]] static bool is_explorer_window(HWND window, DWORD& process_id);
    };
}

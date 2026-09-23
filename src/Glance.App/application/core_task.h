#pragma once

#include <windows.h>

namespace glance::app
{
    enum class CoreAccessResult : DWORD
    {
        success,
        failed,
        cancelled,
        unsafe_location,
        administrator_required,
    };

    // Call task operations on a COM-initialized background thread.
    [[nodiscard]] CoreAccessResult register_core_task() noexcept;
    [[nodiscard]] bool remove_core_tasks() noexcept;
    [[nodiscard]] bool run_core_task() noexcept;
    [[nodiscard]] CoreAccessResult repair_core_task(HWND owner) noexcept;
}

#pragma once
#include <windows.h>

namespace glance::app
{
    enum class CoreAccessResult : DWORD { success, failed, cancelled, unsafe_location, administrator_required };
    [[nodiscard]] CoreAccessResult repair_access_service(HWND owner) noexcept;
}

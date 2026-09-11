#pragma once

#include <windows.h>
#include <cstdint>
#include <string_view>

namespace glance::components
{
    inline bool preview_owner_has_exited(std::wstring_view owner) noexcept
    {
        if (owner.empty()) return false;
        std::uint64_t pid{};
        for (const wchar_t digit : owner)
        {
            if (digit < L'0' || digit > L'9') return false;
            pid = pid * 10 + static_cast<unsigned>(digit - L'0');
            if (pid > MAXDWORD) return false;
        }
        if (pid == 0) return false;
        const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
        if (process == nullptr)
        {
            return GetLastError() == ERROR_INVALID_PARAMETER;
        }
        const bool exited = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
        CloseHandle(process);
        return exited;
    }
}

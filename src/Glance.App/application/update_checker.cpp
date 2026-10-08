#include "pch.h"
#include "update_checker.h"
#include <shellapi.h>

namespace glance::app
{
    UpdateLaunchStatus launch_update_installer(
        const std::filesystem::path& installer_path) noexcept
    {
        try
        {
            if (!std::filesystem::is_regular_file(installer_path))
            {
                return UpdateLaunchStatus::failed;
            }
            constexpr wchar_t parameters[] =
                L"/SP- /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /GLANCEUPDATE";
            SHELLEXECUTEINFOW execute{ sizeof(execute) };
            execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
            execute.lpFile = installer_path.c_str();
            execute.lpParameters = parameters;
            execute.lpDirectory = installer_path.parent_path().c_str();
            execute.nShow = SW_SHOWNORMAL;
            if (!ShellExecuteExW(&execute))
            {
                return GetLastError() == ERROR_CANCELLED
                    ? UpdateLaunchStatus::cancelled
                    : UpdateLaunchStatus::failed;
            }
            if (execute.hProcess == nullptr)
            {
                return UpdateLaunchStatus::failed;
            }

            const DWORD wait_result = WaitForSingleObject(execute.hProcess, INFINITE);
            DWORD exit_code = ERROR_GEN_FAILURE;
            const bool exited = wait_result == WAIT_OBJECT_0 &&
                GetExitCodeProcess(execute.hProcess, &exit_code) != FALSE;
            CloseHandle(execute.hProcess);
            if (!exited)
            {
                return UpdateLaunchStatus::failed;
            }
            if (exit_code == 0)
            {
                return UpdateLaunchStatus::launched;
            }
            return exit_code == 2 || exit_code == 5
                ? UpdateLaunchStatus::cancelled
                : UpdateLaunchStatus::failed;
        }
        catch (...)
        {
            return UpdateLaunchStatus::failed;
        }
    }
}

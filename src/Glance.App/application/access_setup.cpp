#include "pch.h"
#include "access_setup.h"
#include "glance/contracts/access_runtime.h"
#include <shellapi.h>

namespace glance::app
{
    CoreAccessResult repair_access_service(HWND owner) noexcept
    {
        try
        {
            namespace access = contracts::access;
            if (access::installation_mode() != access::InstallationMode::installed) return CoreAccessResult::success;
            const auto directory = access::executable_directory();
            if (!access::protected_installation(directory)) return CoreAccessResult::unsafe_location;
            const auto executable = directory / L"Glance.AccessService.exe";
            SHELLEXECUTEINFOW execute{sizeof(execute)};
            execute.hwnd = owner;
            execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
            execute.lpVerb = L"runas";
            execute.lpFile = executable.c_str();
            execute.lpParameters = L"--install";
            execute.lpDirectory = directory.c_str();
            execute.nShow = SW_HIDE;
            if (!ShellExecuteExW(&execute)) return GetLastError() == ERROR_CANCELLED ? CoreAccessResult::cancelled : CoreAccessResult::failed;
            winrt::handle process(execute.hProcess);
            DWORD code{};
            if (!process || WaitForSingleObject(process.get(), 30000) != WAIT_OBJECT_0 || !GetExitCodeProcess(process.get(), &code))
                return CoreAccessResult::failed;
            return code == ERROR_SUCCESS ? CoreAccessResult::success :
                code == ERROR_ACCESS_DENIED ? CoreAccessResult::administrator_required : CoreAccessResult::failed;
        }
        catch (...) { return CoreAccessResult::failed; }
    }
}

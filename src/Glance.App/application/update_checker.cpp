#include "pch.h"
#include "update_checker.h"

#include <shellapi.h>

#include <optional>
#include <string>

namespace
{
    constexpr wchar_t uninstall_key_path[] =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
        L"{F4A2E1FC-BA77-4A24-83BF-A1D5B90A3E13}_is1";

    std::filesystem::path executable_directory()
    {
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size())
        {
            return {};
        }
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

    std::optional<std::wstring> registry_string(HKEY key, wchar_t const* name)
    {
        DWORD type{};
        DWORD bytes{};
        if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
            (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t))
        {
            return std::nullopt;
        }

        std::wstring value(bytes / sizeof(wchar_t), L'\0');
        if (RegQueryValueExW(
                key,
                name,
                nullptr,
                &type,
                reinterpret_cast<BYTE*>(value.data()),
                &bytes) != ERROR_SUCCESS)
        {
            return std::nullopt;
        }
        while (!value.empty() && value.back() == L'\0')
        {
            value.pop_back();
        }
        if (type != REG_EXPAND_SZ)
        {
            return value;
        }

        const DWORD expanded_size = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
        if (expanded_size == 0)
        {
            return std::nullopt;
        }
        std::wstring expanded(expanded_size, L'\0');
        if (ExpandEnvironmentStringsW(value.c_str(), expanded.data(), expanded_size) == 0)
        {
            return std::nullopt;
        }
        while (!expanded.empty() && expanded.back() == L'\0')
        {
            expanded.pop_back();
        }
        return expanded;
    }

    bool paths_equal(const std::filesystem::path& left, const std::filesystem::path& right)
    {
        const auto normalized_left = std::filesystem::absolute(left).lexically_normal().wstring();
        const auto normalized_right = std::filesystem::absolute(right).lexically_normal().wstring();
        return CompareStringOrdinal(
                   normalized_left.c_str(),
                   static_cast<int>(normalized_left.size()),
                   normalized_right.c_str(),
                   static_cast<int>(normalized_right.size()),
                   TRUE) == CSTR_EQUAL;
    }
}

namespace glance::app
{
    bool managed_installation() noexcept
    {
        try
        {
            HKEY raw_key{};
            if (RegOpenKeyExW(
                    HKEY_LOCAL_MACHINE,
                    uninstall_key_path,
                    0,
                    KEY_QUERY_VALUE | KEY_WOW64_64KEY,
                    &raw_key) != ERROR_SUCCESS)
            {
                return false;
            }
            const auto install_location = registry_string(raw_key, L"InstallLocation");
            RegCloseKey(raw_key);
            return install_location && !install_location->empty() &&
                paths_equal(*install_location, executable_directory());
        }
        catch (...)
        {
            return false;
        }
    }

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

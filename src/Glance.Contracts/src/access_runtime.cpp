#include "glance/contracts/access_runtime.h"
#include "glance/contracts/access_protocol.h"

#include <aclapi.h>
#include <sddl.h>
#include <memory>
#include <vector>

namespace
{
    struct CloseHandleDeleter { void operator()(void* p) const noexcept { if (p && p != INVALID_HANDLE_VALUE) CloseHandle(p); } };
    struct LocalDeleter { void operator()(void* p) const noexcept { LocalFree(p); } };
    struct ServiceDeleter { void operator()(SC_HANDLE p) const noexcept { if (p) CloseServiceHandle(p); } };
    using Handle = std::unique_ptr<void, CloseHandleDeleter>;
    using Local = std::unique_ptr<void, LocalDeleter>;
    using Service = std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, ServiceDeleter>;

    bool privileged_sid(PSID sid)
    {
        if (IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid)) return true;
        PSID installer{};
        if (!ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464", &installer)) return false;
        Local cleanup(installer);
        return EqualSid(sid, installer) != FALSE;
    }

    bool protected_path(const std::filesystem::path& path, bool ancestor = false)
    {
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        PSECURITY_DESCRIPTOR descriptor{};
        PSID owner{};
        PACL acl{};
        const auto status = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &acl, nullptr, &descriptor);
        Local cleanup(descriptor);
        if (status != ERROR_SUCCESS || !owner || !acl || !privileged_sid(owner)) return false;
        const DWORD dangerous = (path == path.root_path() ? 0 : DELETE) | WRITE_DAC | WRITE_OWNER |
            GENERIC_WRITE | GENERIC_ALL | FILE_DELETE_CHILD |
            (ancestor ? 0 : FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES);
        for (DWORD i = 0; i < acl->AceCount; ++i)
        {
            void* entry{};
            if (!GetAce(acl, i, &entry)) return false;
            const auto header = static_cast<ACE_HEADER*>(entry);
            if (header->AceFlags & INHERIT_ONLY_ACE) continue;
            if (header->AceType == ACCESS_ALLOWED_ACE_TYPE)
            {
                const auto ace = static_cast<ACCESS_ALLOWED_ACE*>(entry);
                if ((ace->Mask & dangerous) && !privileged_sid(&ace->SidStart)) return false;
            }
            else if (header->AceType != ACCESS_DENIED_ACE_TYPE) return false;
        }
        return true;
    }

    Service open_service(DWORD rights)
    {
        Service manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
        return Service(manager ? OpenServiceW(manager.get(), glance::contracts::access::service_name, rights) : nullptr);
    }
}

namespace glance::contracts::access
{
    std::filesystem::path executable_directory()
    {
        std::wstring path(32768, L'\0');
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size()) return {};
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

    bool same_directory(const std::filesystem::path& left, const std::filesystem::path& right) noexcept
    {
        Handle a(CreateFileW(left.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        Handle b(CreateFileW(right.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        BY_HANDLE_FILE_INFORMATION first{}, second{};
        return a.get() != INVALID_HANDLE_VALUE && b.get() != INVALID_HANDLE_VALUE &&
            GetFileInformationByHandle(a.get(), &first) && GetFileInformationByHandle(b.get(), &second) &&
            first.dwVolumeSerialNumber == second.dwVolumeSerialNumber &&
            first.nFileIndexHigh == second.nFileIndexHigh && first.nFileIndexLow == second.nFileIndexLow;
    }

    bool protected_installation(const std::filesystem::path& directory) noexcept
    {
        try
        {
            if (!protected_path(directory)) return false;
            for (auto parent = directory.parent_path(); !parent.empty(); parent = parent.parent_path())
            {
                if (!protected_path(parent, true)) return false;
                if (parent == parent.parent_path()) break;
            }
            for (const auto& entry : std::filesystem::directory_iterator(directory))
            {
                if (!protected_path(entry.path())) return false;
            }
            const auto sources = directory / L"sources";
            if (std::filesystem::exists(sources))
                for (const auto& entry : std::filesystem::recursive_directory_iterator(sources))
                    if (!protected_path(entry.path())) return false;
            return true;
        }
        catch (...) { return false; }
    }

    InstallationMode installation_mode() noexcept
    {
        try
        {
            const auto directory = executable_directory();
            wchar_t location[32768]{};
            DWORD size = sizeof(location);
            const auto status = RegGetValueW(HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{F4A2E1FC-BA77-4A24-83BF-A1D5B90A3E13}_is1",
                L"InstallLocation", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, location, &size);
            if (status == ERROR_SUCCESS && same_directory(directory, location)) return InstallationMode::installed;
            if (std::filesystem::is_regular_file(directory / L"Glance.installed")) return InstallationMode::installed;
            if (std::filesystem::is_regular_file(directory / L"Glance.portable")) return InstallationMode::portable;
        }
        catch (...) {}
        return InstallationMode::unknown;
    }

    DWORD start_service() noexcept
    {
        try
        {
            auto service = open_service(SERVICE_QUERY_CONFIG | SERVICE_START);
            if (!service) return GetLastError();
            DWORD size{};
            QueryServiceConfigW(service.get(), nullptr, 0, &size);
            if (!size) return GetLastError();
            std::vector<std::byte> storage(size);
            auto configuration = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(storage.data());
            if (!QueryServiceConfigW(service.get(), configuration, size, &size)) return GetLastError();
            const auto expected = L"\"" + (executable_directory() / L"Glance.AccessService.exe").wstring() + L"\"";
            if (_wcsicmp(configuration->lpBinaryPathName, expected.c_str()) != 0 ||
                _wcsicmp(configuration->lpServiceStartName, L"LocalSystem") != 0) return ERROR_ACCESS_DENIED;
            if (!StartServiceW(service.get(), 0, nullptr))
            {
                const auto error = GetLastError();
                if (error != ERROR_SERVICE_ALREADY_RUNNING) return error;
            }
            return ERROR_SUCCESS;
        }
        catch (...) { return ERROR_GEN_FAILURE; }
    }

    bool service_process(DWORD process_id) noexcept
    {
        auto service = open_service(SERVICE_QUERY_STATUS);
        SERVICE_STATUS_PROCESS status{};
        DWORD size{};
        return service && QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO,
            reinterpret_cast<BYTE*>(&status), sizeof(status), &size) &&
            status.dwCurrentState == SERVICE_RUNNING && status.dwProcessId == process_id;
    }

    DWORD configure_service(bool uninstall) noexcept
    {
        try
        {
            Service manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE));
            if (!manager) return GetLastError();
            if (!uninstall && !protected_installation(executable_directory())) return ERROR_ACCESS_DENIED;
            Service service(OpenServiceW(manager.get(), service_name, SERVICE_ALL_ACCESS));
            if (service)
            {
                SERVICE_STATUS status{};
                if (!QueryServiceStatus(service.get(), &status)) return GetLastError();
                if (status.dwCurrentState != SERVICE_STOPPED && status.dwCurrentState != SERVICE_STOP_PENDING &&
                    !ControlService(service.get(), SERVICE_CONTROL_STOP, &status)) return GetLastError();
                const auto deadline = GetTickCount64() + 15000;
                while (QueryServiceStatus(service.get(), &status) && status.dwCurrentState != SERVICE_STOPPED)
                {
                    if (GetTickCount64() >= deadline) return ERROR_TIMEOUT;
                    Sleep(50);
                }
                if (uninstall) return DeleteService(service.get()) ? ERROR_SUCCESS : GetLastError();
            }
            else if (uninstall)
                return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST ? ERROR_SUCCESS : GetLastError();
            const auto command = L"\"" + (executable_directory() / L"Glance.AccessService.exe").wstring() + L"\"";
            if (!service)
                service.reset(CreateServiceW(manager.get(), service_name, L"Glance preview access", SERVICE_ALL_ACCESS,
                    SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
                    command.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
            else if (!ChangeServiceConfigW(service.get(), SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START,
                SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr, nullptr, L"LocalSystem", nullptr, nullptr))
                return GetLastError();
            if (!service) return GetLastError();
            PSECURITY_DESCRIPTOR descriptor{};
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;CCLCRP;;;IU)", SDDL_REVISION_1, &descriptor, nullptr))
                return GetLastError();
            Local cleanup(descriptor);
            if (!SetServiceObjectSecurity(service.get(), DACL_SECURITY_INFORMATION, descriptor))
                return GetLastError();
            return ERROR_SUCCESS;
        }
        catch (...) { return ERROR_GEN_FAILURE; }
    }

    bool transfer(HANDLE pipe, void* buffer, DWORD bytes, bool write, DWORD timeout, HANDLE cancel) noexcept
    {
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event) return false;
        const auto deadline = GetTickCount64() + timeout;
        auto data = static_cast<std::byte*>(buffer);
        while (bytes)
        {
            OVERLAPPED operation{};
            operation.hEvent = event.get();
            ResetEvent(event.get());
            DWORD count{};
            const BOOL started = write ? WriteFile(pipe, data, bytes, &count, &operation) : ReadFile(pipe, data, bytes, &count, &operation);
            if (!started)
            {
                if (GetLastError() != ERROR_IO_PENDING) return false;
                const auto now = GetTickCount64();
                HANDLE events[]{event.get(), cancel};
                const auto wait = now >= deadline ? WAIT_TIMEOUT : WaitForMultipleObjects(cancel ? 2 : 1,
                    events, FALSE, timeout == INFINITE ? INFINITE : static_cast<DWORD>(deadline - now));
                if (wait != WAIT_OBJECT_0)
                {
                    CancelIoEx(pipe, &operation);
                    WaitForSingleObject(event.get(), INFINITE);
                    SetLastError(ERROR_TIMEOUT);
                    return false;
                }
                if (!GetOverlappedResult(pipe, &operation, &count, FALSE)) return false;
            }
            if (!count) return false;
            data += count;
            bytes -= count;
        }
        return true;
    }

    bool process_image(HANDLE process, const std::filesystem::path& expected) noexcept
    {
        try
        {
            wchar_t path[32768]{};
            DWORD size = ARRAYSIZE(path);
            return QueryFullProcessImageNameW(process, 0, path, &size) && same_directory(path, expected);
        }
        catch (...) { return false; }
    }

    std::wstring process_sid(HANDLE process)
    {
        HANDLE raw{};
        if (!OpenProcessToken(process, TOKEN_QUERY, &raw)) return {};
        Handle token(raw);
        DWORD size{};
        GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
        std::vector<std::byte> storage(size);
        if (!size || !GetTokenInformation(token.get(), TokenUser, storage.data(), size, &size)) return {};
        PWSTR text{};
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &text)) return {};
        Local cleanup(text);
        return text;
    }

    bool system_process(HANDLE process) noexcept
    {
        try { return process_sid(process) == L"S-1-5-18"; }
        catch (...) { return false; }
    }
}

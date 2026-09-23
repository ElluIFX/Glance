#include "pch.h"
#include "core_task.h"
#include "glance/contracts/diagnostics.h"

#include <aclapi.h>
#include <sddl.h>
#include <taskschd.h>
#include <comdef.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <filesystem>
#include <vector>

#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "comsuppw.lib")
#pragma comment(lib, "bcrypt.lib")

namespace
{
    using glance::app::CoreAccessResult;
    constexpr wchar_t folder_name[] = L"\\Glance";
    constexpr wchar_t trusted_installer[] = L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";

    struct LocalFreeDeleter { void operator()(void* value) const noexcept { LocalFree(value); } };
    using LocalMemory = std::unique_ptr<void, LocalFreeDeleter>;
    struct ComApartment
    {
        HRESULT status{ CoInitializeEx(nullptr, COINIT_MULTITHREADED) };
        ComApartment() { if (status != RPC_E_CHANGED_MODE) winrt::check_hresult(status); }
        ~ComApartment() { if (SUCCEEDED(status)) CoUninitialize(); }
    };

    std::filesystem::path app_path()
    {
        std::wstring path(32768, L'\0');
        const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (size == 0 || size >= path.size()) winrt::throw_last_error();
        path.resize(size);
        return std::filesystem::path(path).lexically_normal();
    }

    std::wstring user_sid()
    {
        HANDLE raw{};
        winrt::check_bool(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw));
        winrt::handle token(raw);
        DWORD size{};
        GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
        std::vector<std::byte> storage(size);
        winrt::check_bool(GetTokenInformation(token.get(), TokenUser, storage.data(), size, &size));
        PWSTR text{};
        winrt::check_bool(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &text));
        LocalMemory cleanup(text);
        return text;
    }

    bool administrator()
    {
        BYTE storage[SECURITY_MAX_SID_SIZE]{};
        DWORD size = sizeof(storage);
        winrt::check_bool(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, storage, &size));
        BOOL member{};
        winrt::check_bool(CheckTokenMembership(nullptr, storage, &member));
        return member != FALSE;
    }

    std::wstring account_sid(const wchar_t* account)
    {
        if (std::wstring_view(account).starts_with(L"S-1-")) return account;
        DWORD sid_size{}, domain_size{};
        SID_NAME_USE use{};
        LookupAccountNameW(nullptr, account, nullptr, &sid_size, nullptr, &domain_size, &use);
        std::vector<std::byte> sid(sid_size);
        std::wstring domain(domain_size, L'\0');
        winrt::check_bool(LookupAccountNameW(nullptr, account, sid.data(), &sid_size, domain.data(), &domain_size, &use));
        PWSTR text{};
        winrt::check_bool(ConvertSidToStringSidW(sid.data(), &text));
        LocalMemory cleanup(text);
        return text;
    }

    bool privileged_sid(PSID sid)
    {
        if (IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid)) return true;
        PSID installer{};
        winrt::check_bool(ConvertStringSidToSidW(trusted_installer, &installer));
        LocalMemory cleanup(installer);
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
        LocalMemory cleanup(descriptor);
        if (status != ERROR_SUCCESS || !owner || !acl || !privileged_sid(owner)) return false;
        // A volume root cannot itself be deleted; deleting its children still matters.
        const DWORD dangerous = (path == path.root_path() ? 0 : DELETE) | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL |
            FILE_DELETE_CHILD | (ancestor ? 0 : FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES);
        for (DWORD index = 0; index < acl->AceCount; ++index)
        {
            void* raw{};
            if (!GetAce(acl, index, &raw)) return false;
            const auto header = static_cast<ACE_HEADER*>(raw);
            if (header->AceFlags & INHERIT_ONLY_ACE) continue;
            if (header->AceType == ACCESS_ALLOWED_ACE_TYPE)
            {
                const auto ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
                if ((ace->Mask & dangerous) && !privileged_sid(&ace->SidStart)) return false;
            }
            else if (header->AceType != ACCESS_DENIED_ACE_TYPE)
            {
                // Complex grants require an administrator to choose a conventional protected location.
                return false;
            }
        }
        return true;
    }

    bool protected_installation(bool recursive)
    {
        const auto executable = app_path();
        const auto directory = executable.parent_path();
        if (!protected_path(executable) || !protected_path(directory / L"Glance.Core.exe") ||
            !protected_path(directory)) return false;
        for (auto parent = directory.parent_path(); !parent.empty(); parent = parent.parent_path())
        {
            if (!protected_path(parent, true)) return false;
            if (parent == parent.parent_path()) break;
        }
        if (recursive)
        {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(directory))
                if (!protected_path(entry.path())) return false;
        }
        return true;
    }

    std::wstring task_name()
    {
        auto path = app_path().parent_path().wstring();
        CharLowerBuffW(path.data(), static_cast<DWORD>(path.size()));
        BYTE digest[32]{};
        winrt::check_bool(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
            reinterpret_cast<PUCHAR>(path.data()), static_cast<ULONG>(path.size() * sizeof(wchar_t)),
            digest, sizeof(digest)) >= 0);
        std::wstring name = L"Core-" + user_sid() + L"-";
        constexpr wchar_t digits[] = L"0123456789abcdef";
        for (const auto byte : digest) { name += digits[byte >> 4]; name += digits[byte & 15]; }
        return name;
    }

    winrt::com_ptr<ITaskService> service()
    {
        winrt::com_ptr<ITaskService> result;
        winrt::check_hresult(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(result.put())));
        winrt::check_hresult(result->Connect(_variant_t{}, _variant_t{}, _variant_t{}, _variant_t{}));
        return result;
    }

    bool matches_path(IRegisteredTask* task)
    {
        winrt::com_ptr<ITaskDefinition> definition;
        winrt::com_ptr<IActionCollection> actions;
        winrt::com_ptr<IAction> action;
        winrt::check_hresult(task->get_Definition(definition.put()));
        winrt::check_hresult(definition->get_Actions(actions.put()));
        LONG count{};
        winrt::check_hresult(actions->get_Count(&count));
        if (count != 1) return false;
        winrt::check_hresult(actions->get_Item(1, action.put()));
        const auto execute = action.try_as<IExecAction>();
        if (!execute) return false;
        _bstr_t path, arguments;
        winrt::check_hresult(execute->get_Path(path.GetAddress()));
        winrt::check_hresult(execute->get_Arguments(arguments.GetAddress()));
        return path.length() && arguments.length() && std::wstring_view(arguments) == L"--scheduled" &&
            _wcsicmp(path, (app_path().parent_path() / L"Glance.Core.exe").c_str()) == 0;
    }

    bool trusted_task(IRegisteredTask* task)
    {
        _bstr_t security;
        winrt::check_hresult(task->GetSecurityDescriptor(OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, security.GetAddress()));
        PSECURITY_DESCRIPTOR raw{};
        winrt::check_bool(ConvertStringSecurityDescriptorToSecurityDescriptorW(security, SDDL_REVISION_1, &raw, nullptr));
        LocalMemory descriptor(raw);
        PSID owner{};
        BOOL defaulted{}, present{};
        PACL acl{};
        if (!GetSecurityDescriptorOwner(raw, &owner, &defaulted) || !owner || !privileged_sid(owner) ||
            !GetSecurityDescriptorDacl(raw, &present, &acl, &defaulted) || !present || !acl) return false;
        for (DWORD index = 0; index < acl->AceCount; ++index)
        {
            void* entry{};
            if (!GetAce(acl, index, &entry)) return false;
            const auto header = static_cast<ACE_HEADER*>(entry);
            if (header->AceType == ACCESS_ALLOWED_ACE_TYPE)
            {
                const auto ace = static_cast<ACCESS_ALLOWED_ACE*>(entry);
                if ((ace->Mask & (GENERIC_ALL | GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA |
                    DELETE | WRITE_DAC | WRITE_OWNER)) && !privileged_sid(&ace->SidStart)) return false;
            }
            else if (header->AceType != ACCESS_DENIED_ACE_TYPE) return false;
        }
        winrt::com_ptr<ITaskDefinition> definition;
        winrt::com_ptr<IPrincipal> principal;
        winrt::check_hresult(task->get_Definition(definition.put()));
        winrt::check_hresult(definition->get_Principal(principal.put()));
        _bstr_t user;
        TASK_LOGON_TYPE logon{};
        TASK_RUNLEVEL_TYPE level{};
        winrt::check_hresult(principal->get_UserId(user.GetAddress()));
        winrt::check_hresult(principal->get_LogonType(&logon));
        winrt::check_hresult(principal->get_RunLevel(&level));
        return user.length() && account_sid(user) == user_sid() &&
            logon == TASK_LOGON_INTERACTIVE_TOKEN && level == TASK_RUNLEVEL_HIGHEST;
    }
}

namespace glance::app
{
    CoreAccessResult register_core_task() noexcept
    {
        try
        {
            const ComApartment apartment;
            if (!administrator()) return CoreAccessResult::administrator_required;
            if (!protected_installation(true)) return CoreAccessResult::unsafe_location;
            const auto scheduler = service();
            winrt::com_ptr<ITaskFolder> root, folder;
            winrt::check_hresult(scheduler->GetFolder(_bstr_t(L"\\"), root.put()));
            const auto sid = user_sid();
            const auto security = L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGX;;;" + sid + L")";
            // The folder grants traversal to authenticated users, but only administrators may modify it.
            const _bstr_t folder_security(L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGX;;;AU)");
            if (FAILED(scheduler->GetFolder(_bstr_t(folder_name), folder.put())))
                winrt::check_hresult(root->CreateFolder(_bstr_t(L"Glance"), _variant_t(folder_security), folder.put()));
            winrt::check_hresult(folder->SetSecurityDescriptor(folder_security, 0));
            winrt::com_ptr<ITaskDefinition> definition;
            winrt::check_hresult(scheduler->NewTask(0, definition.put()));
            winrt::com_ptr<IPrincipal> principal;
            winrt::check_hresult(definition->get_Principal(principal.put()));
            winrt::check_hresult(principal->put_UserId(_bstr_t(sid.c_str())));
            winrt::check_hresult(principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN));
            winrt::check_hresult(principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST));
            winrt::com_ptr<ITaskSettings> settings;
            winrt::check_hresult(definition->get_Settings(settings.put()));
            winrt::check_hresult(settings->put_AllowDemandStart(VARIANT_TRUE));
            winrt::check_hresult(settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE));
            winrt::check_hresult(settings->put_StopIfGoingOnBatteries(VARIANT_FALSE));
            winrt::check_hresult(settings->put_RunOnlyIfIdle(VARIANT_FALSE));
            winrt::check_hresult(settings->put_ExecutionTimeLimit(_bstr_t(L"PT0S")));
            winrt::check_hresult(settings->put_MultipleInstances(TASK_INSTANCES_PARALLEL));
            winrt::check_hresult(settings->put_Enabled(VARIANT_TRUE));
            winrt::com_ptr<IActionCollection> actions;
            winrt::com_ptr<IAction> action;
            winrt::check_hresult(definition->get_Actions(actions.put()));
            winrt::check_hresult(actions->Create(TASK_ACTION_EXEC, action.put()));
            const auto execute = action.as<IExecAction>();
            const auto directory = app_path().parent_path();
            winrt::check_hresult(execute->put_Path(_bstr_t((directory / L"Glance.Core.exe").c_str())));
            winrt::check_hresult(execute->put_Arguments(_bstr_t(L"--scheduled")));
            winrt::check_hresult(execute->put_WorkingDirectory(_bstr_t(directory.c_str())));
            winrt::com_ptr<IRegisteredTask> task;
            winrt::check_hresult(folder->RegisterTaskDefinition(_bstr_t(task_name().c_str()), definition.get(),
                TASK_CREATE_OR_UPDATE | TASK_DONT_ADD_PRINCIPAL_ACE, _variant_t(sid.c_str()), _variant_t{},
                TASK_LOGON_INTERACTIVE_TOKEN, _variant_t(security.c_str()), task.put()));
            return CoreAccessResult::success;
        }
        catch (...) { return CoreAccessResult::failed; }
    }

    bool run_core_task() noexcept
    {
        try
        {
            const ComApartment apartment;
            if (!protected_installation(false)) return false;
            const auto scheduler = service();
            winrt::com_ptr<ITaskFolder> folder;
            winrt::check_hresult(scheduler->GetFolder(_bstr_t(folder_name), folder.put()));
            winrt::com_ptr<IRegisteredTask> task;
            winrt::check_hresult(folder->GetTask(_bstr_t(task_name().c_str()), task.put()));
            VARIANT_BOOL enabled{};
            winrt::check_hresult(task->get_Enabled(&enabled));
            if (!enabled || !matches_path(task.get()) || !trusted_task(task.get())) return false;
            DWORD session{};
            winrt::check_bool(ProcessIdToSessionId(GetCurrentProcessId(), &session));
            winrt::com_ptr<IRunningTask> running;
            winrt::check_hresult(task->RunEx(_variant_t{}, TASK_RUN_USE_SESSION_ID,
                static_cast<LONG>(session), _bstr_t{}, running.put()));
            return true;
        }
        catch (...) { return false; }
    }

    bool remove_core_tasks() noexcept
    {
        try
        {
            const ComApartment apartment;
            const auto scheduler = service();
            winrt::com_ptr<ITaskFolder> folder;
            const auto status = scheduler->GetFolder(_bstr_t(folder_name), folder.put());
            if (status == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || status == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) return true;
            winrt::check_hresult(status);
            winrt::com_ptr<IRegisteredTaskCollection> tasks;
            winrt::check_hresult(folder->GetTasks(TASK_ENUM_HIDDEN, tasks.put()));
            LONG count{};
            winrt::check_hresult(tasks->get_Count(&count));
            for (LONG index = count; index > 0; --index)
            {
                winrt::com_ptr<IRegisteredTask> task;
                winrt::check_hresult(tasks->get_Item(_variant_t(index), task.put()));
                _bstr_t name;
                winrt::check_hresult(task->get_Name(name.GetAddress()));
                if (std::wstring_view(name).starts_with(L"Core-") && matches_path(task.get()))
                    winrt::check_hresult(folder->DeleteTask(name, 0));
            }
            return true;
        }
        catch (...) { return false; }
    }

    CoreAccessResult repair_core_task(HWND owner) noexcept
    {
        try
        {
            if (!protected_installation(true)) return CoreAccessResult::unsafe_location;
            HANDLE raw{};
            winrt::check_bool(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw));
            winrt::handle token(raw);
            TOKEN_ELEVATION_TYPE type{};
            DWORD size{};
            winrt::check_bool(GetTokenInformation(token.get(), TokenElevationType, &type, sizeof(type), &size));
            if (type == TokenElevationTypeDefault && !administrator()) return CoreAccessResult::administrator_required;
            const auto path = app_path();
            SHELLEXECUTEINFOW execute{ sizeof(execute) };
            execute.hwnd = owner;
            execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
            execute.lpVerb = L"runas";
            execute.lpFile = path.c_str();
            execute.lpParameters = L"--register-core-task";
            execute.nShow = SW_HIDE;
            if (!ShellExecuteExW(&execute))
                return GetLastError() == ERROR_CANCELLED ? CoreAccessResult::cancelled : CoreAccessResult::failed;
            winrt::handle process(execute.hProcess);
            if (!process || WaitForSingleObject(process.get(), 120000) != WAIT_OBJECT_0) return CoreAccessResult::failed;
            DWORD code{};
            if (!GetExitCodeProcess(process.get(), &code) || code > static_cast<DWORD>(CoreAccessResult::administrator_required))
                return CoreAccessResult::failed;
            return static_cast<CoreAccessResult>(code);
        }
        catch (...) { return CoreAccessResult::failed; }
    }
}

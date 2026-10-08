#include "glance/contracts/access_protocol.h"
#include "glance/contracts/access_runtime.h"
#include "unique_handle.h"
#include "../version.h"
#include <sddl.h>
#include <aclapi.h>
#include <objbase.h>
#include <memory>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
    namespace access = glance::contracts::access;
    using glance::core::unique_handle;
    SERVICE_STATUS_HANDLE status_handle{};
    unique_handle stop_event;
    std::mutex sessions_mutex;
    std::vector<DWORD> sessions;

    void report(DWORD state, DWORD error = NO_ERROR)
    {
        SERVICE_STATUS status{SERVICE_WIN32_OWN_PROCESS, state,
            state == SERVICE_RUNNING ? static_cast<DWORD>(SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0UL,
            error, 0, state == SERVICE_STOP_PENDING ? 1UL : 0UL, state == SERVICE_STOP_PENDING ? 5000UL : 0UL};
        SetServiceStatus(status_handle, &status);
    }

    void WINAPI control(DWORD code)
    {
        if (code == SERVICE_CONTROL_STOP || code == SERVICE_CONTROL_SHUTDOWN)
        {
            report(SERVICE_STOP_PENDING);
            SetEvent(stop_event.get());
        }
    }

    bool enable_privilege(HANDLE token, const wchar_t* name)
    {
        TOKEN_PRIVILEGES privilege{};
        privilege.PrivilegeCount = 1;
        if (!LookupPrivilegeValueW(nullptr, name, &privilege.Privileges[0].Luid)) return false;
        privilege.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        SetLastError(ERROR_SUCCESS);
        return AdjustTokenPrivileges(token, FALSE, &privilege, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
    }

    DWORD spawn(HANDLE client, DWORD session, const access::BootstrapRequest& request,
        access::BootstrapReply& reply, unique_handle& job, unique_handle& child)
    {
        if (request.signature != access::magic || request.protocol != access::version) return ERROR_REVISION_MISMATCH;
        if (wcsnlen_s(request.app_version, ARRAYSIZE(request.app_version)) == ARRAYSIZE(request.app_version) ||
            wcscmp(request.app_version, GLANCE_VERSION_WSTRING) != 0) return ERROR_OLD_WIN_VERSION;
        if (request.notification_message != WM_APP + 1) return ERROR_INVALID_PARAMETER;
        if (!request.notification_window) return ERROR_INVALID_PARAMETER;
        HANDLE raw{};
        if (!DuplicateHandle(client, reinterpret_cast<HANDLE>(request.mapping), GetCurrentProcess(),
            &raw, FILE_MAP_READ | FILE_MAP_WRITE, TRUE, 0)) return GetLastError();
        unique_handle mapping(raw);
        auto state = static_cast<access::SharedState*>(MapViewOfFile(mapping.get(), FILE_MAP_READ | FILE_MAP_WRITE,
            0, 0, sizeof(access::SharedState)));
        if (!state) return GetLastError();
        UnmapViewOfFile(state);
        if (!DuplicateHandle(GetCurrentProcess(), client, GetCurrentProcess(), &raw,
            PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, TRUE, 0)) return GetLastError();
        unique_handle inherited_client(raw);

        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &raw)) return GetLastError();
        unique_handle system_token(raw);
        if (!enable_privilege(system_token.get(), SE_TCB_NAME) ||
            !enable_privilege(system_token.get(), SE_ASSIGNPRIMARYTOKEN_NAME) ||
            !enable_privilege(system_token.get(), SE_INCREASE_QUOTA_NAME)) return ERROR_PRIVILEGE_NOT_HELD;
        if (!CreateRestrictedToken(system_token.get(), DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr, 0, nullptr, &raw))
            return GetLastError();
        unique_handle token(raw);
        if (!SetTokenInformation(token.get(), TokenSessionId, &session, sizeof(session))) return GetLastError();
        BYTE sid[SECURITY_MAX_SID_SIZE]{};
        DWORD sid_size = sizeof(sid);
        if (!CreateWellKnownSid(WinHighLabelSid, nullptr, sid, &sid_size)) return GetLastError();
        TOKEN_MANDATORY_LABEL integrity{{sid, SE_GROUP_INTEGRITY}};
        if (!SetTokenInformation(token.get(), TokenIntegrityLevel, &integrity, sizeof(integrity) + sid_size))
            return GetLastError();
        GUID id{};
        if (FAILED(CoCreateGuid(&id))) return ERROR_GEN_FAILURE;
        wchar_t guid[40]{};
        StringFromGUID2(id, guid, ARRAYSIZE(guid));
        const std::wstring endpoint = std::wstring(L"\\\\.\\pipe\\Glance.AccessHost.") + guid;
        wcscpy_s(reply.endpoint, endpoint.c_str());
        const auto executable = access::executable_directory() / L"Glance.AccessHost.exe";
        std::wstring command = L"\"" + executable.wstring() + L"\" " +
            std::to_wstring(reinterpret_cast<UINT_PTR>(mapping.get())) + L" " +
            std::to_wstring(reinterpret_cast<UINT_PTR>(inherited_client.get())) + L" " +
            std::to_wstring(request.notification_window) + L" " + endpoint;
        SIZE_T attribute_size{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
        std::vector<std::byte> storage(attribute_size);
        auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size)) return GetLastError();
        struct Cleanup { LPPROC_THREAD_ATTRIBUTE_LIST p; ~Cleanup() { DeleteProcThreadAttributeList(p); } } cleanup{attributes};
        HANDLE inherited[]{mapping.get(), inherited_client.get()};
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
            sizeof(inherited), nullptr, nullptr)) return GetLastError();
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        wchar_t desktop[] = L"winsta0\\default";
        startup.StartupInfo.lpDesktop = desktop;
        startup.lpAttributeList = attributes;
        job.reset(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            return GetLastError();
        PROCESS_INFORMATION process{};
        if (!CreateProcessAsUserW(token.get(), executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr,
            access::executable_directory().c_str(), &startup.StartupInfo, &process)) return GetLastError();
        child.reset(process.hProcess);
        unique_handle thread(process.hThread);
        PSECURITY_DESCRIPTOR child_security{};
        const auto child_acl = L"D:P(A;;GA;;;SY)(A;;0x101000;;;" + access::process_sid(client) + L")";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(child_acl.c_str(), SDDL_REVISION_1, &child_security, nullptr))
        { TerminateProcess(child.get(), ERROR_ACCESS_DENIED); return ERROR_ACCESS_DENIED; }
        struct ChildSecurityCleanup { void* p; ~ChildSecurityCleanup() { LocalFree(p); } } child_cleanup{child_security};
        if (!SetKernelObjectSecurity(child.get(), DACL_SECURITY_INFORMATION, child_security))
        { TerminateProcess(child.get(), ERROR_ACCESS_DENIED); return ERROR_ACCESS_DENIED; }
        if (!AssignProcessToJobObject(job.get(), child.get()))
        {
            const auto error = GetLastError();
            TerminateProcess(child.get(), error);
            return error;
        }
        if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) return GetLastError();
        reply.host_pid = process.dwProcessId;
        return ERROR_SUCCESS;
    }

    void serve(unique_handle pipe) noexcept
    {
        DWORD session{};
        bool registered{};
        try
        {
            ULONG pid{};
            unique_handle client;
            access::BootstrapReply reply{};
            access::BootstrapRequest request{};
            if (!GetNamedPipeClientProcessId(pipe.get(), &pid)) return;
            client.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, pid));
            if (!client || !access::process_image(client.get(), access::executable_directory() / L"Glance.Core.exe") ||
                !ProcessIdToSessionId(pid, &session) || !session || access::process_sid(client.get()).empty()) return;
            HANDLE raw{};
            DWORD token_session{}, size{};
            if (!OpenProcessToken(client.get(), TOKEN_QUERY, &raw)) return;
            unique_handle token(raw);
            if (!GetTokenInformation(token.get(), TokenSessionId, &token_session, sizeof(token_session), &size) ||
                token_session != session) return;
            if (!access::transfer(pipe.get(), &request, sizeof(request), false, 2000, stop_event.get())) return;
            {
                std::lock_guard lock(sessions_mutex);
                if (std::find(sessions.begin(), sessions.end(), session) != sessions.end()) reply.error = ERROR_BUSY;
                else if (sessions.size() >= 16) reply.error = ERROR_TOO_MANY_SESS;
                else { sessions.push_back(session); registered = true; }
            }
            unique_handle job, child;
            if (!reply.error) reply.error = spawn(client.get(), session, request, reply, job, child);
            if (reply.error == ERROR_OLD_WIN_VERSION)
                swprintf_s(reply.endpoint, L"Client %.31s; service %s", request.app_version, GLANCE_VERSION_WSTRING);
            if (!access::transfer(pipe.get(), &reply, sizeof(reply), true, 2000, stop_event.get()) || reply.error) goto done;
            {
                unique_handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                OVERLAPPED pending{};
                pending.hEvent = event.get();
                BYTE unexpected{};
                DWORD count{};
                if (ReadFile(pipe.get(), &unexpected, 1, &count, &pending) || GetLastError() != ERROR_IO_PENDING) goto done;
                HANDLE waits[]{stop_event.get(), client.get(), child.get(), event.get()};
                WaitForMultipleObjects(ARRAYSIZE(waits), waits, FALSE, INFINITE);
                CancelIoEx(pipe.get(), &pending);
                WaitForSingleObject(event.get(), INFINITE);
            }
        done:;
        }
        catch (...) {}
        if (registered)
        {
            std::lock_guard lock(sessions_mutex);
            std::erase(sessions, session);
        }
    }

    void WINAPI service_main(DWORD, PWSTR*) noexcept
    {
        status_handle = RegisterServiceCtrlHandlerW(access::service_name, control);
        if (!status_handle) return;
        try
        {
            stop_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!stop_event || !access::system_process(GetCurrentProcess()) ||
                !access::protected_installation(access::executable_directory()))
            { report(SERVICE_STOPPED, ERROR_ACCESS_DENIED); return; }
            PSECURITY_DESCRIPTOR descriptor{};
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)S:(ML;;NW;;;ME)", SDDL_REVISION_1, &descriptor, nullptr))
            { report(SERVICE_STOPPED, GetLastError()); return; }
            struct LocalCleanup { void* p; ~LocalCleanup() { LocalFree(p); } } cleanup{descriptor};
            SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
            struct Worker { std::jthread thread; std::shared_ptr<std::atomic_bool> done; };
            std::vector<Worker> workers;
            struct StopWorkers { ~StopWorkers() { SetEvent(stop_event.get()); } } stop_workers;
            auto idle_since = GetTickCount64();
            bool first = true;
            report(SERVICE_RUNNING);
            while (WaitForSingleObject(stop_event.get(), 0) != WAIT_OBJECT_0)
            {
                std::erase_if(workers, [](Worker& worker) {
                    if (!worker.done->load()) return false;
                    worker.thread.join(); return true;
                });
                unique_handle pipe(CreateNamedPipeW(access::service_pipe, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                    (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0), PIPE_TYPE_BYTE | PIPE_REJECT_REMOTE_CLIENTS,
                    16, 4096, 4096, 0, &security));
                if (!pipe) break;
                first = false;
                unique_handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                OVERLAPPED pending{}; pending.hEvent = event.get();
                const BOOL connected = ConnectNamedPipe(pipe.get(), &pending);
                const auto error = connected ? ERROR_SUCCESS : GetLastError();
                bool accepted = connected || error == ERROR_PIPE_CONNECTED;
                while (!accepted && error == ERROR_IO_PENDING)
                {
                    HANDLE waits[]{event.get(), stop_event.get()};
                    const auto wait = WaitForMultipleObjects(2, waits, FALSE, 1000);
                    if (wait == WAIT_OBJECT_0) { accepted = true; break; }
                    if (wait != WAIT_TIMEOUT) break;
                    {
                        std::lock_guard lock(sessions_mutex);
                        if (!sessions.empty()) idle_since = GetTickCount64();
                    }
                    if (GetTickCount64() - idle_since >= 30000) break;
                    std::erase_if(workers, [](Worker& worker) {
                        if (!worker.done->load()) return false;
                        worker.thread.join(); return true;
                    });
                }
                if (!accepted)
                {
                    CancelIoEx(pipe.get(), &pending);
                    if (error == ERROR_IO_PENDING) WaitForSingleObject(event.get(), INFINITE);
                    break;
                }
                if (workers.size() >= 32) continue;
                auto done = std::make_shared<std::atomic_bool>(false);
                workers.push_back({std::jthread([connection = std::move(pipe), done]() mutable {
                    serve(std::move(connection)); done->store(true);
                }), done});
                idle_since = GetTickCount64();
            }
            SetEvent(stop_event.get());
            for (auto& worker : workers) worker.thread.join();
            report(SERVICE_STOPPED);
        }
        catch (...) { report(SERVICE_STOPPED, ERROR_GEN_FAILURE); }
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR command, int)
{
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    if (wcscmp(command, L"--install") == 0) return static_cast<int>(access::configure_service(false));
    if (wcscmp(command, L"--uninstall") == 0) return static_cast<int>(access::configure_service(true));
    SERVICE_TABLE_ENTRYW table[]{{const_cast<PWSTR>(access::service_name), service_main}, {nullptr, nullptr}};
    return StartServiceCtrlDispatcherW(table) ? 0 : static_cast<int>(GetLastError());
}

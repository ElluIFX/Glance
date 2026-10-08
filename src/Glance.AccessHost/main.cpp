#include "access_wire.h"
#include "keyboard_hook.h"
#include "unique_handle.h"
#include "glance/contracts/access_runtime.h"
#include "glance/contracts/diagnostics.h"
#include <shellapi.h>
#include <sddl.h>
#include <thread>
#include <winrt/base.h>

namespace
{
    namespace access = glance::contracts::access;
    using namespace glance::core;
    constexpr UINT refresh_message = WM_APP + 2;
    access::SharedState* shared{};
    KeyboardHookService* hook{};
    HANDLE client_process{};
    HWND host_window{};
    DWORD session_id{};

    bool same_session(HWND window)
    {
        DWORD pid{}, session{};
        GetWindowThreadProcessId(window, &pid);
        return pid && ProcessIdToSessionId(pid, &session) && session == session_id;
    }

    LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_TIMER)
        {
            if (WaitForSingleObject(client_process, 0) != WAIT_TIMEOUT) PostMessageW(window, WM_CLOSE, 0, 0);
            shared->host_tick.store(GetTickCount64(), std::memory_order_release);
            shared->hook_events.store(hook->event_count(), std::memory_order_relaxed);
            return 0;
        }
        if (message == refresh_message) { static_cast<void>(hook->refresh()); return 0; }
        if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
        if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void serve(std::wstring endpoint) noexcept
    {
        try
        {
            const auto sid = access::process_sid(client_process);
            if (sid.empty()) throw std::runtime_error("Missing client identity");
            PSECURITY_DESCRIPTOR descriptor{};
            const auto acl = L"D:P(A;;GA;;;SY)(A;;GRGW;;;" + sid + L")S:(ML;;NW;;;ME)";
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
                throw std::runtime_error("Access pipe security");
            struct Cleanup { void* p; ~Cleanup() { LocalFree(p); } } cleanup{descriptor};
            SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
            unique_handle pipe(CreateNamedPipeW(endpoint.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &security));
            unique_handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!pipe || !event) throw std::runtime_error("Access pipe creation");
            OVERLAPPED pending{}; pending.hEvent = event.get();
            if (!ConnectNamedPipe(pipe.get(), &pending))
            {
                const auto error = GetLastError();
                if (error == ERROR_IO_PENDING)
                {
                    HANDLE waits[]{event.get(), client_process};
                    const auto wait = WaitForMultipleObjects(2, waits, FALSE, 10000);
                    if (wait != WAIT_OBJECT_0)
                    {
                        CancelIoEx(pipe.get(), &pending);
                        WaitForSingleObject(event.get(), INFINITE);
                        throw std::runtime_error("Access client connection timeout");
                    }
                }
                else if (error != ERROR_PIPE_CONNECTED) throw std::runtime_error("Access client connection");
            }
            ULONG pid{};
            if (!GetNamedPipeClientProcessId(pipe.get(), &pid) || pid != GetProcessId(client_process))
                throw std::runtime_error("Access client mismatch");
            winrt::init_apartment(winrt::apartment_type::single_threaded);
            struct ComCleanup { ~ComCleanup() { winrt::uninit_apartment(); } } com_cleanup;
            ExplorerSelectionService selections;
            while (WaitForSingleObject(client_process, 0) == WAIT_TIMEOUT)
            {
                access::Header header{};
                if (!access::transfer(pipe.get(), &header, sizeof(header), false, INFINITE, client_process)) break;
                if (header.signature != access::magic || header.size > access::maximum_payload || header.flags) break;
                std::string payload(header.size, '\0');
                if (!access::transfer(pipe.get(), payload.data(), header.size, false, 2000, client_process)) break;
                std::string result;
                switch (header.operation)
                {
                case access::Operation::selection:
                {
                    if (!payload.empty()) throw std::runtime_error("Unexpected selection payload");
                    glance::contracts::SelectionSnapshot snapshot;
                    if (same_session(GetForegroundWindow())) snapshot = selections.query_foreground();
                    const auto suppress = selections.consume_gallery_selection_sync(snapshot);
                    result = access_wire::selection(snapshot, suppress);
                    break;
                }
                case access::Operation::gallery:
                {
                    const auto command = access_wire::gallery_command(payload);
                    if (command.source_window && !same_session(reinterpret_cast<HWND>(command.source_window)))
                        throw std::runtime_error("Gallery session mismatch");
                    result = access_wire::gallery(selections.handle_gallery_command(command,
                        [] { return WaitForSingleObject(client_process, 0) != WAIT_TIMEOUT; }, [] {}));
                    break;
                }
                case access::Operation::refresh_hook:
                    if (!payload.empty()) throw std::runtime_error("Unexpected hook payload");
                    PostMessageW(host_window, refresh_message, 0, 0);
                    break;
                default: throw std::runtime_error("Unknown access operation");
                }
                header.size = static_cast<std::uint32_t>(result.size());
                if (!access::transfer(pipe.get(), &header, sizeof(header), true, 2000, client_process) ||
                    !access::transfer(pipe.get(), result.data(), header.size, true, 2000, client_process)) break;
            }
        }
        catch (...) { glance::contracts::log_event(L"Privileged access session ended"); }
        PostMessageW(host_window, WM_CLOSE, 0, 0);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    glance::contracts::initialize_diagnostics(L"Glance.AccessHost");
    int count{};
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return ERROR_INVALID_PARAMETER;
    struct Cleanup { PWSTR* p; ~Cleanup() { LocalFree(p); } } cleanup{arguments};
    if (count != 5 || !access::system_process(GetCurrentProcess())) return ERROR_ACCESS_DENIED;
    UINT_PTR values[3]{};
    for (int i = 0; i < 3; ++i)
    {
        wchar_t* end{};
        values[i] = _wcstoui64(arguments[i + 1], &end, 10);
        if (!values[i] || !end || *end) return ERROR_INVALID_PARAMETER;
    }
    unique_handle mapping(reinterpret_cast<HANDLE>(values[0]));
    unique_handle client(reinterpret_cast<HANDLE>(values[1]));
    client_process = client.get();
    DWORD window_pid{};
    GetWindowThreadProcessId(reinterpret_cast<HWND>(values[2]), &window_pid);
    if (window_pid != GetProcessId(client.get())) return ERROR_ACCESS_DENIED;
    if (!access::process_image(client.get(), access::executable_directory() / L"Glance.Core.exe") ||
        !ProcessIdToSessionId(GetCurrentProcessId(), &session_id)) return ERROR_ACCESS_DENIED;
    shared = static_cast<access::SharedState*>(MapViewOfFile(mapping.get(), FILE_MAP_READ | FILE_MAP_WRITE,
        0, 0, sizeof(access::SharedState)));
    if (!shared) return static_cast<int>(GetLastError());
    struct MapCleanup { ~MapCleanup() { UnmapViewOfFile(shared); } } map_cleanup;
    WNDCLASSW window_class{};
    window_class.hInstance = instance;
    window_class.lpfnWndProc = window_proc;
    window_class.lpszClassName = L"Glance.AccessHost.Message";
    if (!RegisterClassW(&window_class)) return static_cast<int>(GetLastError());
    host_window = CreateWindowExW(0, window_class.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (!host_window) return static_cast<int>(GetLastError());
    KeyboardHookService keyboard(reinterpret_cast<HWND>(values[2]), WM_APP + 1, shared->input);
    hook = &keyboard;
    if (!keyboard.start()) { DestroyWindow(host_window); return ERROR_INVALID_HOOK_HANDLE; }
    SetTimer(host_window, 1, 50, nullptr);
    std::thread worker(serve, std::wstring(arguments[4]));
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    shared->input.enabled.store(false, std::memory_order_release);
    keyboard.stop();
    // The service owns the process job and bounds shutdown of blocked shell calls.
    if (WaitForSingleObject(worker.native_handle(), 2000) != WAIT_OBJECT_0) ExitProcess(ERROR_TIMEOUT);
    worker.join();
    return 0;
}

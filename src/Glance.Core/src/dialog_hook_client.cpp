#include "dialog_hook_client.h"
#include "dialog_broker_protocol.h"
#include "dialog_hook_api.h"
#include "glance/contracts/diagnostics.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    constexpr UINT query_timeout_ms = 50;
    constexpr ULONGLONG broker_response_timeout_ms = 250;

    void log_hook_failure(std::wstring_view reason)
    {
        static ULONGLONG previous_timestamp{};
        const ULONGLONG now = GetTickCount64();
        if (previous_timestamp == 0 || now - previous_timestamp >= 2000)
        {
            previous_timestamp = now;
            glance::contracts::log_event(L"Dialog hook unavailable: " + std::wstring(reason) + L".");
        }
    }

    std::wstring adjacent_module_path(std::wstring_view filename)
    {
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size())
        {
            return {};
        }
        path.resize(length);
        const auto separator = path.find_last_of(L"\\/");
        if (separator == std::wstring::npos)
        {
            return {};
        }
        path.resize(separator + 1);
        path.append(filename);
        return path;
    }

    bool write_exact(HANDLE pipe, const void* buffer, DWORD size)
    {
        const auto* source = static_cast<const std::byte*>(buffer);
        DWORD offset{};
        while (offset < size)
        {
            DWORD written{};
            if (!WriteFile(pipe, source + offset, size - offset, &written, nullptr) || written == 0)
            {
                return false;
            }
            offset += written;
        }
        return true;
    }

    bool read_exact_with_timeout(
        HANDLE pipe,
        HANDLE process,
        void* buffer,
        DWORD size,
        ULONGLONG timeout_ms)
    {
        auto* destination = static_cast<std::byte*>(buffer);
        DWORD offset{};
        const ULONGLONG started = GetTickCount64();
        while (offset < size)
        {
            DWORD available{};
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
            {
                return false;
            }
            if (available == 0)
            {
                if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0 ||
                    GetTickCount64() - started >= timeout_ms)
                {
                    return false;
                }
                Sleep(1);
                continue;
            }

            const DWORD requested = (std::min)(available, size - offset);
            DWORD received{};
            if (!ReadFile(pipe, destination + offset, requested, &received, nullptr) || received == 0)
            {
                return false;
            }
            offset += received;
        }
        return true;
    }
}

namespace glance::core
{
    DialogHookClient::~DialogHookClient()
    {
        detach_native_hook();
        stop_broker();
        if (module_ != nullptr)
        {
            FreeLibrary(module_);
        }
    }

    DialogHookClient::TargetArchitecture DialogHookClient::target_architecture(DWORD process_id) const
    {
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
        if (process == nullptr)
        {
            log_hook_failure(L"the target process cannot be queried");
            return TargetArchitecture::unsupported;
        }

        USHORT process_machine{};
        USHORT native_machine{};
        const BOOL queried = IsWow64Process2(process, &process_machine, &native_machine);
        CloseHandle(process);
        if (!queried)
        {
            log_hook_failure(L"the target process architecture cannot be determined");
            return TargetArchitecture::unsupported;
        }
        static_cast<void>(native_machine);
        return process_machine == IMAGE_FILE_MACHINE_I386
            ? TargetArchitecture::x86
            : TargetArchitecture::native;
    }

    bool DialogHookClient::load_native_hook()
    {
        if (module_ != nullptr)
        {
            return hook_proc_ != nullptr && prepare_ != nullptr && read_path_ != nullptr && query_message_ != 0;
        }

        const auto path = adjacent_module_path(L"Glance.DialogHook.dll");
        if (path.empty())
        {
            log_hook_failure(L"the Core module path is unavailable");
            return false;
        }

        module_ = LoadLibraryExW(
            path.c_str(),
            nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module_ == nullptr)
        {
            log_hook_failure(L"Glance.DialogHook.dll could not be loaded (error " +
                std::to_wstring(GetLastError()) + L")");
            return false;
        }

        hook_proc_ = reinterpret_cast<glance::dialog_hook::HookProcedure>(
            GetProcAddress(module_, glance::dialog_hook::hook_proc_export));
        prepare_ = reinterpret_cast<glance::dialog_hook::PrepareFunction>(
            GetProcAddress(module_, glance::dialog_hook::prepare_export));
        read_path_ = reinterpret_cast<glance::dialog_hook::ReadPathFunction>(
            GetProcAddress(module_, glance::dialog_hook::read_path_export));
        query_message_ = RegisterWindowMessageW(glance::dialog_hook::query_message_name);
        if (hook_proc_ == nullptr || prepare_ == nullptr || read_path_ == nullptr || query_message_ == 0)
        {
            log_hook_failure(L"the native hook exports or query message are unavailable");
            FreeLibrary(module_);
            module_ = nullptr;
            hook_proc_ = nullptr;
            prepare_ = nullptr;
            read_path_ = nullptr;
            query_message_ = 0;
            return false;
        }
        return true;
    }

    bool DialogHookClient::attach_native_hook(DWORD process_id, DWORD thread_id)
    {
        if (native_hook_ != nullptr &&
            native_hooked_process_id_ == process_id &&
            native_hooked_thread_id_ == thread_id)
        {
            return true;
        }

        detach_native_hook();
        if (!load_native_hook())
        {
            return false;
        }

        native_hook_ = SetWindowsHookExW(WH_CALLWNDPROC, hook_proc_, module_, thread_id);
        if (native_hook_ == nullptr)
        {
            log_hook_failure(L"SetWindowsHookExW failed with error " + std::to_wstring(GetLastError()));
            return false;
        }
        native_hooked_process_id_ = process_id;
        native_hooked_thread_id_ = thread_id;
        return true;
    }

    std::wstring DialogHookClient::query_native_hook(
        HWND dialog_window,
        DWORD process_id,
        DWORD thread_id)
    {
        if (!attach_native_hook(process_id, thread_id))
        {
            return {};
        }

        prepare_(process_id, thread_id);
        DWORD_PTR message_result{};
        if (SendMessageTimeoutW(
                dialog_window,
                query_message_,
                0,
                0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK,
                query_timeout_ms,
                &message_result) == 0)
        {
            log_hook_failure(L"the native dialog query timed out or failed");
            return {};
        }

        std::array<wchar_t, 32768> path{};
        const UINT length = read_path_(path.data(), static_cast<UINT>(path.size()));
        return length == 0 ? std::wstring{} : std::wstring(path.data(), length);
    }

    void DialogHookClient::detach_native_hook() noexcept
    {
        if (native_hook_ != nullptr)
        {
            UnhookWindowsHookEx(native_hook_);
            native_hook_ = nullptr;
        }
        native_hooked_process_id_ = 0;
        native_hooked_thread_id_ = 0;
    }

    bool DialogHookClient::start_broker()
    {
        if (broker_process_ &&
            broker_request_pipe_ &&
            broker_response_pipe_ &&
            WaitForSingleObject(broker_process_.get(), 0) == WAIT_TIMEOUT)
        {
            return true;
        }
        stop_broker();

        const auto executable = adjacent_module_path(L"Glance.DialogBroker32.exe");
        if (executable.empty())
        {
            log_hook_failure(L"the 32-bit dialog broker path is unavailable");
            return false;
        }

        SECURITY_ATTRIBUTES security_attributes{
            sizeof(SECURITY_ATTRIBUTES),
            nullptr,
            TRUE
        };
        HANDLE child_request_read{};
        HANDLE parent_request_write{};
        HANDLE parent_response_read{};
        HANDLE child_response_write{};
        if (!CreatePipe(&child_request_read, &parent_request_write, &security_attributes, 0) ||
            !CreatePipe(&parent_response_read, &child_response_write, &security_attributes, 0))
        {
            if (child_request_read != nullptr) CloseHandle(child_request_read);
            if (parent_request_write != nullptr) CloseHandle(parent_request_write);
            if (parent_response_read != nullptr) CloseHandle(parent_response_read);
            if (child_response_write != nullptr) CloseHandle(child_response_write);
            log_hook_failure(L"the 32-bit dialog broker pipes could not be created");
            return false;
        }

        unique_handle request_read(child_request_read);
        unique_handle request_write(parent_request_write);
        unique_handle response_read(parent_response_read);
        unique_handle response_write(child_response_write);
        if (!SetHandleInformation(request_write.get(), HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(response_read.get(), HANDLE_FLAG_INHERIT, 0))
        {
            log_hook_failure(L"the 32-bit dialog broker pipe inheritance could not be restricted");
            return false;
        }

        SIZE_T attribute_size{};
        static_cast<void>(InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size));
        std::vector<std::byte> attribute_storage(attribute_size);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_size))
        {
            log_hook_failure(L"the 32-bit dialog broker attribute list could not be initialized");
            return false;
        }

        HANDLE inherited_handles[]{ request_read.get(), response_write.get() };
        const BOOL attributes_updated = UpdateProcThreadAttribute(
            startup.lpAttributeList,
            0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited_handles,
            sizeof(inherited_handles),
            nullptr,
            nullptr);
        if (!attributes_updated)
        {
            DeleteProcThreadAttributeList(startup.lpAttributeList);
            log_hook_failure(L"the 32-bit dialog broker handle list could not be configured");
            return false;
        }

        std::wstring command_line = L"\"" + executable + L"\" --request-handle=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(request_read.get())) +
            L" --response-handle=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(response_write.get()));
        PROCESS_INFORMATION process_info{};
        const BOOL created = CreateProcessW(
            executable.c_str(),
            command_line.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
            nullptr,
            nullptr,
            &startup.StartupInfo,
            &process_info);
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        if (!created)
        {
            log_hook_failure(L"Glance.DialogBroker32.exe could not be started (error " +
                std::to_wstring(GetLastError()) + L")");
            return false;
        }

        CloseHandle(process_info.hThread);
        broker_process_.reset(process_info.hProcess);
        broker_request_pipe_ = std::move(request_write);
        broker_response_pipe_ = std::move(response_read);
        broker_hook_attached_ = false;
        return true;
    }

    bool DialogHookClient::exchange_with_broker(
        std::uint32_t command,
        HWND dialog_window,
        DWORD process_id,
        DWORD thread_id,
        std::wstring& path)
    {
        path.clear();
        if (!start_broker())
        {
            return false;
        }

        glance::dialog_broker::Request request;
        request.command = static_cast<glance::dialog_broker::Command>(command);
        request.window = reinterpret_cast<std::uintptr_t>(dialog_window);
        request.process_id = process_id;
        request.thread_id = thread_id;
        if (!write_exact(broker_request_pipe_.get(), &request, sizeof(request)))
        {
            log_hook_failure(L"the 32-bit dialog broker request could not be written");
            stop_broker();
            return false;
        }

        glance::dialog_broker::Response response;
        if (!read_exact_with_timeout(
                broker_response_pipe_.get(),
                broker_process_.get(),
                &response,
                sizeof(response),
                broker_response_timeout_ms) ||
            response.magic != glance::dialog_broker::protocol_magic ||
            response.version != glance::dialog_broker::protocol_version ||
            response.path_length > glance::dialog_broker::maximum_path_length)
        {
            log_hook_failure(L"the 32-bit dialog broker response is unavailable or invalid");
            stop_broker();
            return false;
        }

        if (response.path_length != 0)
        {
            path.resize(response.path_length);
            if (!read_exact_with_timeout(
                    broker_response_pipe_.get(),
                    broker_process_.get(),
                    path.data(),
                    response.path_length * static_cast<DWORD>(sizeof(wchar_t)),
                    broker_response_timeout_ms))
            {
                log_hook_failure(L"the 32-bit dialog broker path is unavailable");
                stop_broker();
                path.clear();
                return false;
            }
        }
        return response.status == glance::dialog_broker::Status::success ||
            response.status == glance::dialog_broker::Status::no_selection;
    }

    std::wstring DialogHookClient::query_broker(
        HWND dialog_window,
        DWORD process_id,
        DWORD thread_id)
    {
        std::wstring path;
        if (!exchange_with_broker(
                static_cast<std::uint32_t>(glance::dialog_broker::Command::query),
                dialog_window,
                process_id,
                thread_id,
                path))
        {
            return {};
        }
        broker_hook_attached_ = true;
        return path;
    }

    void DialogHookClient::detach_broker() noexcept
    {
        if (!broker_hook_attached_)
        {
            return;
        }
        std::wstring ignored;
        static_cast<void>(exchange_with_broker(
            static_cast<std::uint32_t>(glance::dialog_broker::Command::detach),
            nullptr,
            0,
            0,
            ignored));
        broker_hook_attached_ = false;
    }

    void DialogHookClient::stop_broker() noexcept
    {
        broker_hook_attached_ = false;
        if (broker_process_ &&
            broker_request_pipe_ &&
            broker_response_pipe_ &&
            WaitForSingleObject(broker_process_.get(), 0) == WAIT_TIMEOUT)
        {
            glance::dialog_broker::Request request;
            request.command = glance::dialog_broker::Command::shutdown;
            static_cast<void>(write_exact(broker_request_pipe_.get(), &request, sizeof(request)));
        }

        broker_request_pipe_.reset();
        broker_response_pipe_.reset();
        if (broker_process_ && WaitForSingleObject(broker_process_.get(), 500) == WAIT_TIMEOUT)
        {
            TerminateProcess(broker_process_.get(), 0);
            static_cast<void>(WaitForSingleObject(broker_process_.get(), 100));
        }
        broker_process_.reset();
    }

    std::wstring DialogHookClient::query(
        HWND dialog_window,
        DWORD process_id,
        DWORD thread_id)
    {
        if (dialog_window == nullptr || process_id == 0 || thread_id == 0)
        {
            return {};
        }

        switch (target_architecture(process_id))
        {
        case TargetArchitecture::native:
            detach_broker();
            return query_native_hook(dialog_window, process_id, thread_id);
        case TargetArchitecture::x86:
            detach_native_hook();
            return query_broker(dialog_window, process_id, thread_id);
        default:
            detach();
            return {};
        }
    }

    void DialogHookClient::detach() noexcept
    {
        detach_native_hook();
        detach_broker();
    }
}

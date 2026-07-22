#include "dialog_broker_protocol.h"
#include "dialog_hook_api.h"

#include <windows.h>
#include <shellapi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <string>
#include <string_view>

namespace
{
    constexpr UINT query_timeout_ms = 50;

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

    bool read_exact(HANDLE pipe, void* buffer, DWORD size)
    {
        auto* destination = static_cast<std::byte*>(buffer);
        DWORD offset{};
        while (offset < size)
        {
            DWORD received{};
            if (!ReadFile(pipe, destination + offset, size - offset, &received, nullptr) || received == 0)
            {
                return false;
            }
            offset += received;
        }
        return true;
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

    HANDLE parse_handle(int argument_count, wchar_t** arguments, std::wstring_view prefix)
    {
        for (int index = 1; index < argument_count; ++index)
        {
            const std::wstring_view argument(arguments[index]);
            if (!argument.starts_with(prefix))
            {
                continue;
            }

            wchar_t* end{};
            const auto value = _wcstoui64(arguments[index] + prefix.size(), &end, 10);
            if (end != arguments[index] + prefix.size() &&
                *end == L'\0' &&
                value <= std::numeric_limits<std::uintptr_t>::max())
            {
                return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value));
            }
        }
        return nullptr;
    }

    class HookController final
    {
    public:
        ~HookController()
        {
            detach();
            if (module_ != nullptr)
            {
                FreeLibrary(module_);
            }
        }

        [[nodiscard]] glance::dialog_broker::Status query(
            HWND dialog_window,
            DWORD process_id,
            DWORD thread_id,
            std::wstring& path)
        {
            path.clear();
            if (dialog_window == nullptr || process_id == 0 || thread_id == 0 || !attach(process_id, thread_id))
            {
                return glance::dialog_broker::Status::hook_failed;
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
                return glance::dialog_broker::Status::hook_failed;
            }

            std::array<wchar_t, 32768> buffer{};
            const UINT length = read_path_(buffer.data(), static_cast<UINT>(buffer.size()));
            if (length == 0)
            {
                return glance::dialog_broker::Status::no_selection;
            }
            path.assign(buffer.data(), length);
            return glance::dialog_broker::Status::success;
        }

        void detach() noexcept
        {
            if (hook_ != nullptr)
            {
                UnhookWindowsHookEx(hook_);
                hook_ = nullptr;
            }
            hooked_process_id_ = 0;
            hooked_thread_id_ = 0;
        }

    private:
        [[nodiscard]] bool load()
        {
            if (module_ != nullptr)
            {
                return hook_proc_ != nullptr && prepare_ != nullptr && read_path_ != nullptr && query_message_ != 0;
            }

            const auto path = adjacent_module_path(L"Glance.DialogHook32.dll");
            if (path.empty())
            {
                return false;
            }
            module_ = LoadLibraryExW(
                path.c_str(),
                nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (module_ == nullptr)
            {
                return false;
            }

            hook_proc_ = reinterpret_cast<glance::dialog_hook::HookProcedure>(
                GetProcAddress(module_, glance::dialog_hook::hook_proc_export));
            prepare_ = reinterpret_cast<glance::dialog_hook::PrepareFunction>(
                GetProcAddress(module_, glance::dialog_hook::prepare_export));
            read_path_ = reinterpret_cast<glance::dialog_hook::ReadPathFunction>(
                GetProcAddress(module_, glance::dialog_hook::read_path_export));
            query_message_ = RegisterWindowMessageW(glance::dialog_hook::query_message_name);
            return hook_proc_ != nullptr && prepare_ != nullptr && read_path_ != nullptr && query_message_ != 0;
        }

        [[nodiscard]] bool attach(DWORD process_id, DWORD thread_id)
        {
            if (hook_ != nullptr &&
                hooked_process_id_ == process_id &&
                hooked_thread_id_ == thread_id)
            {
                return true;
            }

            detach();
            if (!load())
            {
                return false;
            }
            hook_ = SetWindowsHookExW(WH_CALLWNDPROC, hook_proc_, module_, thread_id);
            if (hook_ == nullptr)
            {
                return false;
            }
            hooked_process_id_ = process_id;
            hooked_thread_id_ = thread_id;
            return true;
        }

        HMODULE module_{};
        HHOOK hook_{};
        DWORD hooked_process_id_{};
        DWORD hooked_thread_id_{};
        UINT query_message_{};
        glance::dialog_hook::HookProcedure hook_proc_{};
        glance::dialog_hook::PrepareFunction prepare_{};
        glance::dialog_hook::ReadPathFunction read_path_{};
    };

    int run_broker(HANDLE request_pipe, HANDLE response_pipe)
    {
        HookController hook;
        while (true)
        {
            glance::dialog_broker::Request request;
            if (!read_exact(request_pipe, &request, sizeof(request)))
            {
                return 0;
            }

            glance::dialog_broker::Response response;
            std::wstring path;
            if (request.magic != glance::dialog_broker::protocol_magic ||
                request.version != glance::dialog_broker::protocol_version)
            {
                response.status = glance::dialog_broker::Status::invalid_request;
            }
            else if (request.command == glance::dialog_broker::Command::query)
            {
                response.status = hook.query(
                    reinterpret_cast<HWND>(static_cast<std::uintptr_t>(request.window)),
                    request.process_id,
                    request.thread_id,
                    path);
                response.path_length = static_cast<std::uint32_t>(path.size());
            }
            else if (request.command == glance::dialog_broker::Command::detach)
            {
                hook.detach();
                response.status = glance::dialog_broker::Status::success;
            }
            else if (request.command == glance::dialog_broker::Command::shutdown)
            {
                hook.detach();
                response.status = glance::dialog_broker::Status::success;
            }
            else
            {
                response.status = glance::dialog_broker::Status::invalid_request;
            }

            if (response.path_length > glance::dialog_broker::maximum_path_length ||
                !write_exact(response_pipe, &response, sizeof(response)) ||
                (response.path_length != 0 &&
                 !write_exact(
                     response_pipe,
                     path.data(),
                     response.path_length * static_cast<DWORD>(sizeof(wchar_t)))))
            {
                return 1;
            }
            if (request.command == glance::dialog_broker::Command::shutdown)
            {
                return 0;
            }
        }
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argument_count{};
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (arguments == nullptr)
    {
        return 2;
    }

    const HANDLE request_pipe = parse_handle(argument_count, arguments, L"--request-handle=");
    const HANDLE response_pipe = parse_handle(argument_count, arguments, L"--response-handle=");
    LocalFree(arguments);
    if (request_pipe == nullptr || response_pipe == nullptr)
    {
        return 2;
    }

    const int result = run_broker(request_pipe, response_pipe);
    CloseHandle(request_pipe);
    CloseHandle(response_pipe);
    return result;
}

#include "dialog_hook_client.h"
#include "dialog_hook_api.h"
#include "glance/contracts/diagnostics.h"

#include <array>
#include <string_view>

namespace
{
    constexpr UINT query_timeout_ms = 50;

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
}

namespace glance::core
{
    DialogHookClient::~DialogHookClient()
    {
        detach();
        if (module_ != nullptr)
        {
            FreeLibrary(module_);
        }
    }

    bool DialogHookClient::load()
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
            log_hook_failure(L"the hook exports or query message are unavailable");
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

    bool DialogHookClient::supports_target(DWORD process_id) const
    {
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
        if (process == nullptr)
        {
            log_hook_failure(L"the target process cannot be queried");
            return false;
        }

        USHORT process_machine{};
        USHORT native_machine{};
        const BOOL queried = IsWow64Process2(process, &process_machine, &native_machine);
        CloseHandle(process);
        if (!queried)
        {
            log_hook_failure(L"the target process architecture cannot be determined");
            return false;
        }
        if (process_machine != IMAGE_FILE_MACHINE_UNKNOWN)
        {
            log_hook_failure(L"32-bit file-dialog hosts are not supported by the x64 hook");
            return false;
        }
        return true;
    }

    bool DialogHookClient::attach(DWORD process_id, DWORD thread_id)
    {
        if (hook_ != nullptr &&
            hooked_process_id_ == process_id &&
            hooked_thread_id_ == thread_id)
        {
            return true;
        }

        detach();
        if (!load() || !supports_target(process_id))
        {
            return false;
        }

        hook_ = SetWindowsHookExW(WH_CALLWNDPROC, hook_proc_, module_, thread_id);
        if (hook_ == nullptr)
        {
            log_hook_failure(L"SetWindowsHookExW failed with error " + std::to_wstring(GetLastError()));
            return false;
        }
        hooked_process_id_ = process_id;
        hooked_thread_id_ = thread_id;
        return true;
    }

    std::wstring DialogHookClient::query(
        HWND dialog_window,
        DWORD process_id,
        DWORD thread_id)
    {
        if (dialog_window == nullptr || process_id == 0 || thread_id == 0 ||
            !attach(process_id, thread_id))
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
            log_hook_failure(L"the dialog query timed out or failed");
            return {};
        }

        std::array<wchar_t, 32768> path{};
        const UINT length = read_path_(path.data(), static_cast<UINT>(path.size()));
        return length == 0 ? std::wstring{} : std::wstring(path.data(), length);
    }

    void DialogHookClient::detach() noexcept
    {
        if (hook_ != nullptr)
        {
            UnhookWindowsHookEx(hook_);
            hook_ = nullptr;
        }
        hooked_process_id_ = 0;
        hooked_thread_id_ = 0;
    }
}

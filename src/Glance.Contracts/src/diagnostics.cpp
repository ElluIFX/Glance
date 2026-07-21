#include "glance/contracts/diagnostics.h"

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>

namespace
{
    constexpr wchar_t registry_path[] = L"Software\\Glance\\Diagnostics";
    std::wstring process_name = L"Glance";
    std::mutex log_mutex;

    std::filesystem::path diagnostics_root_directory()
    {
        std::wstring local_app_data(32768, L'\0');
        const DWORD length = GetEnvironmentVariableW(
            L"LOCALAPPDATA", local_app_data.data(), static_cast<DWORD>(local_app_data.size()));
        local_app_data.resize(length < local_app_data.size() ? length : 0);
        std::filesystem::path result = local_app_data.empty()
            ? std::filesystem::temp_directory_path()
            : std::filesystem::path(local_app_data);
        result /= L"Glance";
        return result;
    }

    std::filesystem::path diagnostics_directory(std::wstring_view child)
    {
        auto result = diagnostics_root_directory();
        result /= child;
        std::error_code error;
        std::filesystem::create_directories(result, error);
        return result;
    }

    std::wstring timestamp()
    {
        SYSTEMTIME time{};
        GetLocalTime(&time);
        wchar_t value[32]{};
        swprintf_s(
            value,
            L"%04u%02u%02u-%02u%02u%02u",
            time.wYear,
            time.wMonth,
            time.wDay,
            time.wHour,
            time.wMinute,
            time.wSecond);
        return value;
    }

    LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* exception) noexcept
    {
        if (!glance::contracts::diagnostics_enabled())
        {
            return EXCEPTION_EXECUTE_HANDLER;
        }
        const auto path = diagnostics_directory(L"Dumps") /
            (process_name + L"-" + timestamp() + L".dmp");
        const HANDLE file = CreateFileW(
            path.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            MINIDUMP_EXCEPTION_INFORMATION information{};
            information.ThreadId = GetCurrentThreadId();
            information.ExceptionPointers = exception;
            information.ClientPointers = FALSE;
            MiniDumpWriteDump(
                GetCurrentProcess(),
                GetCurrentProcessId(),
                file,
                MiniDumpNormal,
                &information,
                nullptr,
                nullptr);
            CloseHandle(file);
        }
        glance::contracts::log_event(L"Unhandled exception; crash dump requested.");
        return EXCEPTION_EXECUTE_HANDLER;
    }
}

namespace glance::contracts
{
    bool diagnostics_enabled() noexcept
    {
        DWORD enabled{};
        DWORD size = sizeof(enabled);
        RegGetValueW(
            HKEY_CURRENT_USER,
            registry_path,
            L"Enabled",
            RRF_RT_REG_DWORD,
            nullptr,
            &enabled,
            &size);
        return enabled != 0;
    }

    std::wstring diagnostics_root_path() noexcept
    {
        try
        {
            const auto root = diagnostics_root_directory();
            std::error_code error;
            std::filesystem::create_directories(root / L"Logs", error);
            error.clear();
            std::filesystem::create_directories(root / L"Dumps", error);
            return root.wstring();
        }
        catch (...)
        {
            return {};
        }
    }

    void set_diagnostics_enabled(bool enabled) noexcept
    {
        HKEY key{};
        if (RegCreateKeyExW(
                HKEY_CURRENT_USER,
                registry_path,
                0,
                nullptr,
                0,
                KEY_SET_VALUE,
                nullptr,
                &key,
                nullptr) != ERROR_SUCCESS)
        {
            return;
        }
        const DWORD value = enabled;
        RegSetValueExW(key, L"Enabled", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(key);
    }

    void initialize_diagnostics(std::wstring_view name) noexcept
    {
        process_name = name;
        SetUnhandledExceptionFilter(unhandled_exception_filter);
        log_event(L"Process started.");
    }

    void log_event(std::wstring_view message) noexcept
    {
        if (!diagnostics_enabled())
        {
            return;
        }
        std::scoped_lock lock(log_mutex);
        const auto path = diagnostics_directory(L"Logs") / (process_name + L".log");
        FILE* file{};
        if (_wfopen_s(&file, path.c_str(), L"a, ccs=UTF-8") != 0 || file == nullptr)
        {
            return;
        }
        fwprintf(file, L"[%s] [pid:%lu] %.*s\n", timestamp().c_str(), GetCurrentProcessId(),
            static_cast<int>(message.size()), message.data());
        fclose(file);
    }
}

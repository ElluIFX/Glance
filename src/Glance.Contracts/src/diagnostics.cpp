#include "glance/contracts/diagnostics.h"
#include "glance/contracts/storage.h"

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
        return glance::contracts::storage::data_directory();
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
        try
        {
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
        }
        catch (...) {}
        return EXCEPTION_EXECUTE_HANDLER;
    }
}

namespace glance::contracts
{
    bool diagnostics_enabled() noexcept
    {
        return storage::read_dword(registry_path, L"Enabled", 0) != 0;
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
        storage::Batch key(registry_path);
        const DWORD value = enabled;
        key.set(L"Enabled", REG_DWORD, &value, sizeof(value));
        static_cast<void>(key.commit());
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
        try
        {
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
        catch (...) {}
    }
}

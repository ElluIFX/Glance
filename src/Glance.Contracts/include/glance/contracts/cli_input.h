#pragma once
#include "cli_protocol.h"
#include <atomic>
#include <memory>

namespace glance::cli
{
    inline std::filesystem::path input_root()
    {
        return std::filesystem::temp_directory_path() / L"Glance" / L"CliInput";
    }

    struct InputLease
    {
        std::filesystem::path directory;
        ~InputLease()
        {
            std::error_code error;
            std::filesystem::remove_all(directory, error);
        }
    };

    inline std::shared_ptr<InputLease> create_input_lease()
    {
        static std::atomic_uint64_t sequence{};
        const auto root = input_root();
        std::filesystem::create_directories(root);
        // Only reclaim directories whose owning process has exited.
        std::error_code error;
        unsigned scanned{};
        for (const auto& entry : std::filesystem::directory_iterator(root, error))
        {
            if (++scanned > 1000) break;
            const auto name = entry.path().filename().wstring();
            wchar_t* end{};
            const auto pid = wcstoul(name.c_str(), &end, 10);
            if (!pid || !end || *end != L'-' || pid == GetCurrentProcessId()) continue;
            Handle process(OpenProcess(SYNCHRONIZE, FALSE, pid));
            const auto open_error = GetLastError();
            if ((process.value && WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0) ||
                (!process.value && open_error == ERROR_INVALID_PARAMETER))
                std::filesystem::remove_all(entry.path(), error);
        }
        for (;;)
        {
            const auto directory = root / (std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence++));
            if (std::filesystem::create_directory(directory))
            {
                auto lease = std::make_shared<InputLease>();
                lease->directory = directory;
                return lease;
            }
        }
    }
}

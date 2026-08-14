#include "pch.h"
#include "heic_preview_service.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{
    using namespace glance::contracts::components;

    constexpr std::wstring_view extensions[]{ L".heic", L".heif", L".hif" };
    constexpr DWORD host_timeout_ms = 60U * 1000U;

    struct LeaseRecord
    {
        std::filesystem::path directory;
        std::filesystem::path output;
    };

    std::atomic_bool shutting_down{};
    std::mutex lease_mutex;
    std::unordered_map<std::uint64_t, LeaseRecord> leases;
    std::atomic_uint64_t next_lease{ 1 };
    std::mutex process_mutex;
    std::unordered_set<HANDLE> active_processes;

    std::filesystem::path component_directory() noexcept
    {
        try
        {
            std::vector<wchar_t> buffer(32768, L'\0');
            const auto length = GetModuleFileNameW(
                reinterpret_cast<HMODULE>(&__ImageBase),
                buffer.data(),
                static_cast<DWORD>(buffer.size()));
            if (length == 0 || length >= buffer.size())
            {
                return {};
            }
            return std::filesystem::path(buffer.data()).parent_path();
        }
        catch (...)
        {
            return {};
        }
    }

    std::wstring lower(std::wstring_view value) noexcept
    {
        std::wstring result(value);
        for (auto& character : result)
        {
            if (character >= L'A' && character <= L'Z')
            {
                character = static_cast<wchar_t>(character - L'A' + L'a');
            }
        }
        return result;
    }

    std::wstring quote_argument(std::wstring_view value)
    {
        std::wstring result{ L'"' };
        std::size_t backslashes{};
        for (const auto character : value)
        {
            if (character == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (character == L'"')
            {
                result.append(backslashes * 2 + 1, L'\\');
                result.push_back(character);
                backslashes = 0;
                continue;
            }
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(character);
        }
        result.append(backslashes * 2, L'\\');
        result.push_back(L'"');
        return result;
    }

    void wait_for_process(HANDLE process) noexcept
    {
        WaitForSingleObject(process, host_timeout_ms);
        DWORD exit_code{};
        if (GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE)
        {
            TerminateProcess(process, 1);
        }
    }

    void cleanup_legacy_directories() noexcept
    {
        try
        {
            const auto current_pid = std::to_wstring(GetCurrentProcessId());
            const auto lease_root =
                std::filesystem::temp_directory_path() / L"Glance" / L"HeicPreview";
            std::error_code error;
            for (std::filesystem::directory_iterator iterator(lease_root, error), end;
                 !error && iterator != end;
                 iterator.increment(error))
            {
                if (iterator->is_directory(error) &&
                    iterator->path().filename().wstring() != current_pid)
                {
                    std::filesystem::remove_all(iterator->path(), error);
                }
                error.clear();
            }
        }
        catch (...)
        {
        }
    }
}

namespace glance::components::heic
{
    void initialize() noexcept
    {
        shutting_down.store(false, std::memory_order_release);
        cleanup_legacy_directories();
    }

    bool can_preview(const std::filesystem::path& path) noexcept
    {
        try
        {
            const auto extension = lower(path.extension().wstring());
            for (const auto candidate : extensions)
            {
                if (extension == candidate)
                {
                    return true;
                }
            }
        }
        catch (...)
        {
        }
        return false;
    }

    PreviewResult prepare_preview(
        const std::filesystem::path& path,
        std::uint32_t maximum_dimension) noexcept
    {
        PreviewResult result;
        try
        {
            if (shutting_down.load(std::memory_order_acquire) ||
                !can_preview(path) ||
                maximum_dimension == 0 ||
                maximum_dimension > 8192)
            {
                return result;
            }

            std::filesystem::path directory;
            try
            {
                auto root = std::filesystem::temp_directory_path() /
                    L"Glance" / L"HeicPreview" / std::to_wstring(GetCurrentProcessId());
                const auto token = next_lease.fetch_add(1, std::memory_order_relaxed);
                directory = root / std::to_wstring(token);
                std::filesystem::create_directories(directory);
            }
            catch (...)
            {
                return result;
            }

            const auto output = directory / (path.stem().wstring() + L".png");
            const auto component_directory_value = component_directory();
            if (component_directory_value.empty())
            {
                std::error_code error;
                std::filesystem::remove_all(directory, error);
                return result;
            }
            const auto host = component_directory_value / L"Glance.HeicHost.exe";
            std::error_code host_error;
            if (!std::filesystem::is_regular_file(host, host_error))
            {
                std::error_code error;
                std::filesystem::remove_all(directory, error);
                return result;
            }

            std::wstring command_line = quote_argument(host.wstring()) +
                L" --input " + quote_argument(path.wstring()) +
                L" --output " + quote_argument(output.wstring()) +
                L" --maximum-dimension " + std::to_wstring(maximum_dimension);

            STARTUPINFOW startup{ sizeof(STARTUPINFOW) };
            PROCESS_INFORMATION process_info{};
            if (!CreateProcessW(
                    host.c_str(),
                    command_line.data(),
                    nullptr,
                    nullptr,
                    FALSE,
                    CREATE_NO_WINDOW,
                    nullptr,
                    host.parent_path().c_str(),
                    &startup,
                    &process_info))
            {
                std::error_code error;
                std::filesystem::remove_all(directory, error);
                return result;
            }

            {
                std::lock_guard guard(process_mutex);
                if (shutting_down.load(std::memory_order_acquire))
                {
                    TerminateProcess(process_info.hProcess, 1);
                }
                else
                {
                    active_processes.insert(process_info.hProcess);
                }
            }

            wait_for_process(process_info.hProcess);
            DWORD exit_code{};
            std::error_code output_error;
            const bool succeeded =
                GetExitCodeProcess(process_info.hProcess, &exit_code) &&
                exit_code == ERROR_SUCCESS &&
                std::filesystem::is_regular_file(output, output_error);
            {
                std::lock_guard guard(process_mutex);
                active_processes.erase(process_info.hProcess);
                CloseHandle(process_info.hThread);
                CloseHandle(process_info.hProcess);
            }

            if (!succeeded)
            {
                std::error_code error;
                std::filesystem::remove_all(directory, error);
                return result;
            }

            {
                std::lock_guard guard(lease_mutex);
                leases[result.lease_token = next_lease.fetch_add(1, std::memory_order_relaxed)] =
                    LeaseRecord{ directory, output };
            }
            result.status = PrepareStatus::success;
            result.kind = PreviewContentKind::image;
            result.format = PreviewContentFormat::image_file;
            result.path = output;
        }
        catch (...)
        {
            result.status = PrepareStatus::failed;
            result.kind = PreviewContentKind::none;
            result.format = PreviewContentFormat::none;
            result.path.clear();
            result.lease_token = 0;
        }
        return result;
    }

    void release_preview(std::uint64_t lease_token) noexcept
    {
        LeaseRecord record;
        {
            std::lock_guard guard(lease_mutex);
            const auto found = leases.find(lease_token);
            if (found == leases.end())
            {
                return;
            }
            record = std::move(found->second);
            leases.erase(found);
        }
        std::error_code error;
        std::filesystem::remove_all(record.directory, error);
    }

    void shutdown() noexcept
    {
        shutting_down.store(true, std::memory_order_release);
        std::vector<HANDLE> processes;
        {
            std::lock_guard guard(process_mutex);
            processes.assign(active_processes.begin(), active_processes.end());
            for (const auto process : processes)
            {
                TerminateProcess(process, 1);
            }
        }
        for (const auto process : processes)
        {
            WaitForSingleObject(process, 5000);
        }
        {
            std::lock_guard guard(process_mutex);
            active_processes.clear();
        }
        std::vector<LeaseRecord> records;
        {
            std::lock_guard guard(lease_mutex);
            for (auto& [token, record] : leases)
            {
                records.push_back(std::move(record));
            }
            leases.clear();
        }
        for (auto& record : records)
        {
            std::error_code error;
            std::filesystem::remove_all(record.directory, error);
        }
    }
}
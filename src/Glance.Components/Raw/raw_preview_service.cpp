#include "pch.h"
#include "raw_preview_service.h"
#include "../Common/image_metadata_sidecar.h"

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

    constexpr std::wstring_view extensions[]{
        L".cr2", L".cr3", L".nef", L".nrw", L".arw", L".srf", L".sr2",
        L".orf", L".rw2", L".raf", L".dng", L".pef", L".srw", L".x3f",
        L".erf", L".3fr", L".fff", L".mef", L".mos", L".raw" };
    constexpr DWORD host_timeout_ms = 60U * 1000U;

    enum class HostResult
    {
        success,
        unavailable,
        failed
    };

    struct LeaseRecord
    {
        std::filesystem::path directory;
        std::vector<ImageMetadataEntry> metadata;
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

    HostResult run_host(
        const std::filesystem::path& host,
        const std::wstring& arguments,
        const std::filesystem::path& expected_output) noexcept
    {
        try
        {
            std::wstring command_line = quote_argument(host.wstring()) + L" " + arguments;
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
                return HostResult::failed;
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
            const bool has_exit_code = GetExitCodeProcess(
                process_info.hProcess,
                &exit_code) != FALSE;
            const bool succeeded = has_exit_code &&
                exit_code == ERROR_SUCCESS &&
                std::filesystem::is_regular_file(expected_output, output_error);
            {
                std::lock_guard guard(process_mutex);
                active_processes.erase(process_info.hProcess);
                CloseHandle(process_info.hThread);
                CloseHandle(process_info.hProcess);
            }
            if (succeeded)
            {
                return HostResult::success;
            }
            return has_exit_code && exit_code == ERROR_NOT_SUPPORTED
                ? HostResult::unavailable
                : HostResult::failed;
        }
        catch (...)
        {
            return HostResult::failed;
        }
    }

    void cleanup_legacy_directories() noexcept
    {
        try
        {
            const auto current_pid = std::to_wstring(GetCurrentProcessId());
            const auto lease_root =
                std::filesystem::temp_directory_path() / L"Glance" / L"RawPreview";
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

namespace glance::components::raw
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
                (maximum_dimension != 1024 &&
                 maximum_dimension != 2048 &&
                 maximum_dimension != 4096 &&
                 maximum_dimension != 8192))
            {
                return result;
            }

            std::filesystem::path directory;
            try
            {
                auto root = std::filesystem::temp_directory_path() /
                    L"Glance" / L"RawPreview" / std::to_wstring(GetCurrentProcessId());
                const auto token = next_lease.fetch_add(1, std::memory_order_relaxed);
                directory = root / std::to_wstring(token);
                std::filesystem::create_directories(directory);
            }
            catch (...)
            {
                return result;
            }

            const auto component_directory_value = component_directory();
            if (component_directory_value.empty())
            {
                std::error_code error;
                std::filesystem::remove_all(directory, error);
                return result;
            }
            const auto host = component_directory_value / L"Glance.RawHost.exe";
            std::error_code host_error;
            if (!std::filesystem::is_regular_file(host, host_error))
            {
                std::error_code error;
                std::filesystem::remove_all(directory, error);
                return result;
            }

            const auto thumbnail = directory / L"thumbnail.jpg";
            const auto metadata_output = directory / L"metadata.bin";
            const auto arguments = L"--mode thumbnail --input " +
                quote_argument(path.wstring()) +
                L" --output " + quote_argument(thumbnail.wstring()) +
                L" --metadata-output " + quote_argument(metadata_output.wstring()) +
                L" --maximum-dimension " + std::to_wstring(maximum_dimension);
            const auto host_result = run_host(host, arguments, thumbnail);
            if (host_result != HostResult::success)
            {
                std::error_code error;
                std::filesystem::remove_all(directory, error);
                if (host_result == HostResult::unavailable)
                {
                    result.status = PrepareStatus::unavailable;
                }
                return result;
            }

            std::uint64_t token{};
            {
                std::lock_guard guard(lease_mutex);
                token = next_lease.fetch_add(1, std::memory_order_relaxed);
                leases[token] = LeaseRecord{
                    directory,
                    read_image_metadata_sidecar(metadata_output) };
            }
            result.status = PrepareStatus::success;
            result.kind = PreviewContentKind::image;
            result.format = PreviewContentFormat::image_file;
            result.path = thumbnail;
            result.lease_token = token;
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

    bool query_metadata(
        std::uint64_t lease_token,
        const ImageMetadataSink* sink) noexcept
    {
        if (sink == nullptr || sink->size < sizeof(ImageMetadataSink) ||
            sink->append == nullptr)
        {
            return false;
        }
        try
        {
            std::vector<ImageMetadataEntry> metadata;
            {
                std::lock_guard guard(lease_mutex);
                const auto found = leases.find(lease_token);
                if (found == leases.end())
                {
                    return false;
                }
                metadata = found->second.metadata;
            }
            for (const auto& entry : metadata)
            {
                if ((sink->is_cancelled != nullptr && sink->is_cancelled(sink->context)) ||
                    !sink->append(sink->context, &entry))
                {
                    return false;
                }
            }
            return true;
        }
        catch (...)
        {
            return false;
        }
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

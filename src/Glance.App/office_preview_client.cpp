#include "pch.h"
#include "office_preview_client.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <system_error>
#include <utility>

namespace
{
    using namespace glance::contracts::office;

    std::atomic_uint64_t session_sequence{};

    std::filesystem::path executable_directory()
    {
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

    bool read_exact(HANDLE handle, void* destination, std::size_t size)
    {
        auto* bytes = static_cast<std::byte*>(destination);
        while (size != 0)
        {
            DWORD read{};
            const DWORD request = static_cast<DWORD>(std::min<std::size_t>(size, MAXDWORD));
            if (!ReadFile(handle, bytes, request, &read, nullptr) || read == 0)
            {
                return false;
            }
            bytes += read;
            size -= read;
        }
        return true;
    }

    bool write_exact(HANDLE handle, const void* source, std::size_t size)
    {
        const auto* bytes = static_cast<const std::byte*>(source);
        while (size != 0)
        {
            DWORD written{};
            const DWORD request = static_cast<DWORD>(std::min<std::size_t>(size, MAXDWORD));
            if (!WriteFile(handle, bytes, request, &written, nullptr) || written == 0)
            {
                return false;
            }
            bytes += written;
            size -= written;
        }
        return true;
    }

    template <typename T>
    void append_value(std::vector<std::byte>& output, const T& value)
    {
        const auto* begin = reinterpret_cast<const std::byte*>(&value);
        output.insert(output.end(), begin, begin + sizeof(T));
    }

    std::wstring quote_argument(const std::wstring& value)
    {
        std::wstring result = L"\"";
        std::size_t backslashes{};
        for (const wchar_t character : value)
        {
            if (character == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (character == L'\"')
            {
                result.append(backslashes * 2 + 1, L'\\');
                result.push_back(L'\"');
                backslashes = 0;
                continue;
            }
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(character);
        }
        result.append(backslashes * 2, L'\\');
        result.push_back(L'\"');
        return result;
    }

    std::wstring handle_argument(HANDLE handle)
    {
        return std::to_wstring(reinterpret_cast<std::uintptr_t>(handle));
    }
}

namespace glance::app
{
    OfficePreviewClient::~OfficePreviewClient()
    {
        cancel();
        std::scoped_lock lock(mutex_);
        close_locked();
    }

    bool OfficePreviewClient::open_word(const std::wstring& path)
    {
        std::scoped_lock lock(mutex_);
        if (cancelled_.load(std::memory_order_acquire))
        {
            return false;
        }
        std::error_code error;
        const auto root = std::filesystem::temp_directory_path(error) / L"Glance" / L"Office";
        std::filesystem::create_directories(root, error);
        if (error)
        {
            return false;
        }
        session_directory_ = root /
            (L"emf-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()) + L"-" +
             std::to_wstring(session_sequence.fetch_add(1, std::memory_order_relaxed)));
        cache_directory_ = session_directory_ / L"pages";
        std::filesystem::create_directories(cache_directory_, error);
        if (error)
        {
            close_locked();
            return false;
        }
        staged_input_ = session_directory_ /
            (L"source" + std::filesystem::path(path).extension().wstring());
        if (!std::filesystem::copy_file(
                path,
                staged_input_,
                std::filesystem::copy_options::overwrite_existing,
                error) || error || !launch_word_host())
        {
            close_locked();
            return false;
        }
        Status status{};
        std::vector<std::byte> response;
        if (!transact(Command::open_session, {}, status, response) || status != Status::success)
        {
            close_locked();
            return false;
        }
        return true;
    }

    bool OfficePreviewClient::launch_word_host()
    {
        const auto host_path = executable_directory() / L"Glance.OfficeHost.exe";
        std::error_code error;
        if (!std::filesystem::is_regular_file(host_path, error))
        {
            return false;
        }

        SECURITY_ATTRIBUTES security{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        HANDLE child_request_read{};
        HANDLE parent_request_write{};
        HANDLE parent_response_read{};
        HANDLE child_response_write{};
        if (!CreatePipe(&child_request_read, &parent_request_write, &security, 0) ||
            !CreatePipe(&parent_response_read, &child_response_write, &security, 0))
        {
            if (child_request_read != nullptr) CloseHandle(child_request_read);
            if (parent_request_write != nullptr) CloseHandle(parent_request_write);
            if (parent_response_read != nullptr) CloseHandle(parent_response_read);
            if (child_response_write != nullptr) CloseHandle(child_response_write);
            return false;
        }
        if (!SetHandleInformation(parent_request_write, HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(parent_response_read, HANDLE_FLAG_INHERIT, 0))
        {
            CloseHandle(child_request_read);
            CloseHandle(parent_request_write);
            CloseHandle(parent_response_read);
            CloseHandle(child_response_write);
            return false;
        }

        std::wstring command = quote_argument(host_path.wstring()) + L" --word-session " +
            quote_argument(staged_input_.wstring()) + L" " +
            quote_argument(cache_directory_.wstring()) + L" " +
            handle_argument(child_request_read) + L" " +
            handle_argument(child_response_write);
        SIZE_T attribute_bytes{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
        std::vector<std::byte> attribute_storage(attribute_bytes);
        auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_bytes))
        {
            CloseHandle(child_request_read);
            CloseHandle(parent_request_write);
            CloseHandle(parent_response_read);
            CloseHandle(child_response_write);
            return false;
        }
        std::array inherited_handles{ child_request_read, child_response_write };
        if (!UpdateProcThreadAttribute(
                attributes,
                0,
                PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                inherited_handles.data(),
                sizeof(inherited_handles),
                nullptr,
                nullptr))
        {
            DeleteProcThreadAttributeList(attributes);
            CloseHandle(child_request_read);
            CloseHandle(parent_request_write);
            CloseHandle(parent_response_read);
            CloseHandle(child_response_write);
            return false;
        }

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(
            host_path.c_str(),
            command.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
            nullptr,
            nullptr,
            &startup.StartupInfo,
            &process);
        DeleteProcThreadAttributeList(attributes);
        CloseHandle(child_request_read);
        CloseHandle(child_response_write);
        if (!created)
        {
            CloseHandle(parent_request_write);
            CloseHandle(parent_response_read);
            return false;
        }

        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job != nullptr)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(
                    job,
                    JobObjectExtendedLimitInformation,
                    &limits,
                    sizeof(limits)) ||
                !AssignProcessToJobObject(job, process.hProcess))
            {
                CloseHandle(job);
                job = nullptr;
            }
        }
        ResumeThread(process.hThread);
        CloseHandle(process.hThread);
        request_pipe_ = parent_request_write;
        response_pipe_ = parent_response_read;
        job_ = job;
        {
            std::scoped_lock process_lock(process_handle_mutex_);
            process_ = process.hProcess;
        }
        return true;
    }

    bool OfficePreviewClient::transact(
        Command command,
        const std::vector<std::byte>& request,
        Status& status,
        std::vector<std::byte>& response)
    {
        if (cancelled_.load(std::memory_order_acquire) || request.size() > maximum_payload_size ||
            request_pipe_ == nullptr || response_pipe_ == nullptr)
        {
            return false;
        }
        const RequestHeader header{
            .command = command,
            .payload_size = static_cast<std::uint32_t>(request.size()),
        };
        if (!write_exact(request_pipe_, &header, sizeof(header)) ||
            (!request.empty() && !write_exact(request_pipe_, request.data(), request.size())))
        {
            return false;
        }
        ResponseHeader response_header{};
        if (!read_exact(response_pipe_, &response_header, sizeof(response_header)) ||
            response_header.magic != protocol_magic ||
            response_header.version != protocol_version ||
            response_header.payload_size > maximum_payload_size)
        {
            return false;
        }
        response.resize(response_header.payload_size);
        if (!response.empty() && !read_exact(response_pipe_, response.data(), response.size()))
        {
            return false;
        }
        status = response_header.status;
        return true;
    }

    OfficePageResult OfficePreviewClient::render_page(std::uint32_t page_index)
    {
        std::scoped_lock lock(mutex_);
        OfficePageResult result;
        const PageRequest request{ .page_index = page_index };
        std::vector<std::byte> payload;
        append_value(payload, request);
        std::vector<std::byte> response;
        if (!transact(Command::render_page, payload, result.status, response) ||
            result.status != Status::success || response.size() < sizeof(PageResponse))
        {
            return result;
        }
        PageResponse metadata{};
        std::memcpy(&metadata, response.data(), sizeof(metadata));
        const std::size_t path_bytes =
            static_cast<std::size_t>(metadata.path_characters) * sizeof(wchar_t);
        if (response.size() != sizeof(metadata) + path_bytes)
        {
            result.status = Status::invalid_request;
            return result;
        }
        const auto* path = reinterpret_cast<const wchar_t*>(response.data() + sizeof(metadata));
        result.page_index = metadata.page_index;
        result.page_width_points = metadata.page_width_points;
        result.page_height_points = metadata.page_height_points;
        result.emf_path.assign(path, metadata.path_characters);
        return result;
    }

    OfficePageCountResult OfficePreviewClient::page_count()
    {
        std::scoped_lock lock(mutex_);
        OfficePageCountResult result;
        std::vector<std::byte> response;
        if (!transact(Command::get_page_count, {}, result.status, response) ||
            result.status != Status::success || response.size() != sizeof(PageCountResponse))
        {
            return result;
        }
        PageCountResponse metadata{};
        std::memcpy(&metadata, response.data(), sizeof(metadata));
        result.page_count = metadata.page_count;
        return result;
    }

    void OfficePreviewClient::cancel() noexcept
    {
        cancelled_.store(true, std::memory_order_release);
        std::scoped_lock process_lock(process_handle_mutex_);
        if (process_ != nullptr)
        {
            TerminateProcess(process_, ERROR_CANCELLED);
        }
    }

    void OfficePreviewClient::close_locked() noexcept
    {
        if (request_pipe_ != nullptr)
        {
            CloseHandle(request_pipe_);
            request_pipe_ = nullptr;
        }
        if (response_pipe_ != nullptr)
        {
            CloseHandle(response_pipe_);
            response_pipe_ = nullptr;
        }
        HANDLE process{};
        {
            std::scoped_lock process_lock(process_handle_mutex_);
            process = std::exchange(process_, nullptr);
        }
        if (process != nullptr)
        {
            if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT)
            {
                TerminateProcess(process, ERROR_CANCELLED);
                WaitForSingleObject(process, 5000);
            }
            CloseHandle(process);
        }
        if (job_ != nullptr)
        {
            CloseHandle(job_);
            job_ = nullptr;
        }
        std::error_code error;
        if (!session_directory_.empty())
        {
            std::filesystem::remove_all(session_directory_, error);
        }
        session_directory_.clear();
        staged_input_.clear();
        cache_directory_.clear();
    }
}

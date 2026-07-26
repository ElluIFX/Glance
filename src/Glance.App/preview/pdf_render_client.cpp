#include "pch.h"
#include "pdf_render_client.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <limits>
#include <thread>
#include <utility>

namespace
{
    using namespace glance::contracts::pdf;

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

    void append_utf16(std::vector<std::byte>& output, std::wstring_view value)
    {
        const auto* begin = reinterpret_cast<const std::byte*>(value.data());
        output.insert(output.end(), begin, begin + value.size() * sizeof(wchar_t));
    }

    std::wstring handle_argument(HANDLE handle)
    {
        return std::to_wstring(reinterpret_cast<std::uintptr_t>(handle));
    }
}

namespace glance::app
{
    namespace
    {
        std::mutex warm_client_mutex;
        std::shared_ptr<PdfRenderClient> warm_client;
    }

    PdfRenderClient::~PdfRenderClient()
    {
        cancel();
        std::scoped_lock lock(mutex_);
        close_locked();
    }

    bool PdfRenderClient::start()
    {
        bool stale_process{};
        {
            std::scoped_lock process_lock(process_handle_mutex_);
            if (process_ != nullptr)
            {
                if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT)
                {
                    return true;
                }
                stale_process = true;
            }
        }
        if (stale_process)
        {
            close_locked();
        }
        const auto host_path = executable_directory() / L"Glance.RenderHost.exe";
        std::error_code path_error;
        if (!std::filesystem::is_regular_file(host_path, path_error))
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
        HANDLE mapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            &security,
            PAGE_READWRITE | SEC_RESERVE,
            0,
            shared_bitmap_size,
            nullptr);
        if (mapping == nullptr)
        {
            CloseHandle(child_request_read);
            CloseHandle(parent_request_write);
            CloseHandle(parent_response_read);
            CloseHandle(child_response_write);
            return false;
        }

        std::wstring command = L"\"" + host_path.wstring() + L"\" " +
            handle_argument(child_request_read) + L" " +
            handle_argument(child_response_write) + L" " +
            handle_argument(mapping);
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
            CloseHandle(mapping);
            return false;
        }
        std::array inherited_handles{ child_request_read, child_response_write, mapping };
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
            CloseHandle(mapping);
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
            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
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
            CloseHandle(mapping);
            return false;
        }
        CloseHandle(process.hThread);
        auto* memory = static_cast<std::byte*>(MapViewOfFile(
            mapping,
            FILE_MAP_READ,
            0,
            0,
            shared_bitmap_size));
        if (memory == nullptr)
        {
            TerminateProcess(process.hProcess, ERROR_NOT_ENOUGH_MEMORY);
            WaitForSingleObject(process.hProcess, 1000);
            CloseHandle(process.hProcess);
            CloseHandle(parent_request_write);
            CloseHandle(parent_response_read);
            CloseHandle(mapping);
            return false;
        }
        request_pipe_ = parent_request_write;
        response_pipe_ = parent_response_read;
        mapping_ = mapping;
        bitmap_memory_ = memory;
        {
            std::scoped_lock process_lock(process_handle_mutex_);
            process_ = process.hProcess;
        }
        if (cancelled_.load(std::memory_order_acquire))
        {
            close_locked();
            return false;
        }
        return true;
    }

    void PdfRenderClient::close_locked() noexcept
    {
        if (bitmap_memory_ != nullptr)
        {
            UnmapViewOfFile(bitmap_memory_);
            bitmap_memory_ = nullptr;
        }
        if (mapping_ != nullptr)
        {
            CloseHandle(mapping_);
            mapping_ = nullptr;
        }
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
                WaitForSingleObject(process, 1000);
            }
            CloseHandle(process);
        }
    }

    void PdfRenderClient::cancel() noexcept
    {
        cancelled_.store(true, std::memory_order_release);
        std::scoped_lock process_lock(process_handle_mutex_);
        if (process_ != nullptr)
        {
            TerminateProcess(process_, ERROR_CANCELLED);
        }
    }

    bool PdfRenderClient::prewarm()
    {
        std::scoped_lock lock(mutex_);
        return !cancelled_.load(std::memory_order_acquire) && start();
    }

    bool PdfRenderClient::transact(
        Command command,
        const std::vector<std::byte>& request,
        Status& status,
        std::vector<std::byte>& response)
    {
        if (request.size() > maximum_payload_size)
        {
            status = Status::invalid_request;
            return false;
        }
        if (cancelled_.load(std::memory_order_acquire) || !start())
        {
            status = Status::dependency_missing;
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

    PdfOpenResult PdfRenderClient::open(const std::wstring& path, const std::wstring& password)
    {
        std::scoped_lock lock(mutex_);
        PdfOpenResult result;
        const OpenRequest request{
            .path_characters = static_cast<std::uint32_t>(path.size()),
            .password_characters = static_cast<std::uint32_t>(password.size()),
        };
        std::vector<std::byte> payload;
        append_value(payload, request);
        append_utf16(payload, path);
        append_utf16(payload, password);
        std::vector<std::byte> response;
        if (!transact(Command::open_document, payload, result.status, response) ||
            result.status != Status::success)
        {
            return result;
        }
        if (response.size() < sizeof(OpenResponse))
        {
            result.status = Status::invalid_request;
            return result;
        }
        OpenResponse metadata{};
        std::memcpy(&metadata, response.data(), sizeof(metadata));
        result.page_count = metadata.page_count;
        std::size_t offset = sizeof(metadata);
        result.outline.reserve(metadata.outline_count);
        for (std::uint32_t index = 0; index < metadata.outline_count; ++index)
        {
            if (offset + sizeof(OutlineEntry) > response.size())
            {
                result.status = Status::invalid_request;
                result.outline.clear();
                return result;
            }
            OutlineEntry entry{};
            std::memcpy(&entry, response.data() + offset, sizeof(entry));
            offset += sizeof(entry);
            const std::size_t title_bytes =
                static_cast<std::size_t>(entry.title_characters) * sizeof(wchar_t);
            if (offset + title_bytes > response.size())
            {
                result.status = Status::invalid_request;
                result.outline.clear();
                return result;
            }
            const auto* title = reinterpret_cast<const wchar_t*>(response.data() + offset);
            result.outline.push_back(PdfOutlineEntry{
                .depth = entry.depth,
                .page_index = entry.page_index,
                .title = std::wstring(title, entry.title_characters),
            });
            offset += title_bytes;
        }
        return result;
    }

    PdfRenderResult PdfRenderClient::render(
        std::uint32_t page_index,
        std::uint32_t maximum_width,
        std::uint32_t maximum_height)
    {
        std::scoped_lock lock(mutex_);
        PdfRenderResult result;
        const RenderRequest request{
            .page_index = page_index,
            .maximum_width = maximum_width,
            .maximum_height = maximum_height,
        };
        std::vector<std::byte> payload;
        append_value(payload, request);
        std::vector<std::byte> response;
        if (!transact(Command::render_page, payload, result.status, response) ||
            result.status != Status::success || response.size() != sizeof(RenderResponse))
        {
            return result;
        }
        return consume_render_response(result.status, response);
    }

    PdfRenderResult PdfRenderClient::consume_render_response(
        Status status,
        const std::vector<std::byte>& response)
    {
        PdfRenderResult result;
        result.status = status;
        RenderResponse metadata{};
        std::memcpy(&metadata, response.data(), sizeof(metadata));
        const std::uint64_t byte_count =
            static_cast<std::uint64_t>(metadata.stride) * metadata.pixel_height;
        const std::uint64_t minimum_stride =
            static_cast<std::uint64_t>(metadata.pixel_width) * 4U;
        if (metadata.pixel_width == 0 || metadata.pixel_height == 0 ||
            metadata.pixel_width > maximum_bitmap_dimension ||
            metadata.pixel_height > maximum_bitmap_dimension ||
            metadata.stride < minimum_stride || byte_count > shared_bitmap_size ||
            bitmap_memory_ == nullptr)
        {
            result.status = Status::render_failed;
            return result;
        }
        result.page_index = metadata.page_index;
        result.pixel_width = metadata.pixel_width;
        result.pixel_height = metadata.pixel_height;
        result.stride = metadata.stride;
        result.page_width_points = metadata.page_width_points;
        result.page_height_points = metadata.page_height_points;
        result.pixels.assign(bitmap_memory_, bitmap_memory_ + byte_count);
        return result;
    }

    void prewarm_pdf_render_client()
    {
        auto client = std::make_shared<PdfRenderClient>();
        {
            std::scoped_lock lock(warm_client_mutex);
            if (warm_client != nullptr)
            {
                return;
            }
            warm_client = client;
        }
        std::thread([client] {
            bool ready{};
            try
            {
                ready = client->prewarm();
            }
            catch (...)
            {
            }
            if (!ready)
            {
                std::scoped_lock lock(warm_client_mutex);
                if (warm_client == client)
                {
                    warm_client.reset();
                }
            }
        }).detach();
    }

    std::shared_ptr<PdfRenderClient> acquire_pdf_render_client()
    {
        std::shared_ptr<PdfRenderClient> client;
        {
            std::scoped_lock lock(warm_client_mutex);
            client = std::move(warm_client);
        }
        if (client == nullptr)
        {
            client = std::make_shared<PdfRenderClient>();
        }
        prewarm_pdf_render_client();
        return client;
    }
}

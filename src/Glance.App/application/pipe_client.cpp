#include "pch.h"
#include "pipe_client.h"

#include <chrono>
#include <vector>

namespace glance::app
{
    PipeClient::PipeClient(MessageHandler message_handler, ConnectionHandler connection_handler)
        : message_handler_(std::move(message_handler)), connection_handler_(std::move(connection_handler))
    {
    }

    PipeClient::~PipeClient()
    {
        stop();
    }

    bool PipeClient::start()
    {
        if (thread_.joinable())
        {
            return true;
        }
        stopping_.store(false, std::memory_order_release);
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void PipeClient::stop() noexcept
    {
        stopping_.store(true, std::memory_order_release);
        const HANDLE pipe = pipe_.load(std::memory_order_acquire);
        if (pipe != nullptr && pipe != INVALID_HANDLE_VALUE)
        {
            CancelIoEx(pipe, nullptr);
        }
        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    bool PipeClient::send(
        glance::contracts::MessageType type,
        std::string_view payload,
        std::uint32_t flags)
    {
        if (payload.size() > glance::contracts::maximum_payload_size)
        {
            return false;
        }

        std::scoped_lock lock(write_mutex_);
        const HANDLE pipe = pipe_.load(std::memory_order_acquire);
        if (!connected_.load(std::memory_order_acquire) || pipe == nullptr || pipe == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        glance::contracts::FrameHeader header;
        header.type = type;
        header.flags = flags;
        header.payload_size = static_cast<std::uint32_t>(payload.size());
        header.correlation_id = correlation_id_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!write_exact(pipe, &header, sizeof(header)))
        {
            return false;
        }
        return payload.empty() || write_exact(pipe, payload.data(), static_cast<DWORD>(payload.size()));
    }

    bool PipeClient::read_exact(HANDLE pipe, void* buffer, DWORD size) noexcept
    {
        auto* output = static_cast<std::byte*>(buffer);
        DWORD total{};
        while (total < size && !stopping_.load(std::memory_order_acquire))
        {
            OVERLAPPED operation{};
            operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (operation.hEvent == nullptr)
            {
                return false;
            }
            DWORD read{};
            const BOOL started = ReadFile(pipe, output + total, size - total, &read, &operation);
            if (!started && GetLastError() == ERROR_IO_PENDING)
            {
                while (!stopping_.load(std::memory_order_acquire) &&
                       WaitForSingleObject(operation.hEvent, 100) == WAIT_TIMEOUT)
                {
                }
                if (stopping_.load(std::memory_order_acquire))
                {
                    CancelIoEx(pipe, &operation);
                    WaitForSingleObject(operation.hEvent, INFINITE);
                }
                else if (!GetOverlappedResult(pipe, &operation, &read, FALSE))
                {
                    read = 0;
                }
            }
            else if (!started)
            {
                read = 0;
            }
            CloseHandle(operation.hEvent);
            if (read == 0)
            {
                return false;
            }
            total += read;
        }
        return total == size;
    }

    bool PipeClient::write_exact(HANDLE pipe, const void* buffer, DWORD size) noexcept
    {
        const auto* input = static_cast<const std::byte*>(buffer);
        DWORD total{};
        while (total < size && !stopping_.load(std::memory_order_acquire))
        {
            OVERLAPPED operation{};
            operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (operation.hEvent == nullptr)
            {
                return false;
            }
            DWORD written{};
            const BOOL started = WriteFile(pipe, input + total, size - total, &written, &operation);
            if (!started && GetLastError() == ERROR_IO_PENDING)
            {
                const DWORD wait_result = WaitForSingleObject(operation.hEvent, 2000);
                if (wait_result == WAIT_OBJECT_0)
                {
                    if (!GetOverlappedResult(pipe, &operation, &written, FALSE))
                    {
                        written = 0;
                    }
                }
                else
                {
                    CancelIoEx(pipe, &operation);
                    WaitForSingleObject(operation.hEvent, INFINITE);
                    written = 0;
                }
            }
            else if (!started)
            {
                written = 0;
            }
            CloseHandle(operation.hEvent);
            if (written == 0)
            {
                return false;
            }
            total += written;
        }
        return total == size;
    }

    void PipeClient::run() noexcept
    {
        using namespace std::chrono_literals;
        auto retry_delay = 100ms;
        while (!stopping_.load(std::memory_order_acquire))
        {
            HANDLE pipe = CreateFileW(
                glance::contracts::pipe_name,
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                nullptr);
            if (pipe == INVALID_HANDLE_VALUE)
            {
                WaitNamedPipeW(glance::contracts::pipe_name, 500);
                std::this_thread::sleep_for(retry_delay);
                retry_delay = std::min(retry_delay * 2, 1600ms);
                continue;
            }

            ULONG server_process_id{};
            if (!GetNamedPipeServerProcessId(pipe, &server_process_id))
            {
                CloseHandle(pipe);
                std::this_thread::sleep_for(retry_delay);
                retry_delay = std::min(retry_delay * 2, 1600ms);
                continue;
            }

            retry_delay = 100ms;
            peer_process_id_.store(server_process_id, std::memory_order_release);
            pipe_.store(pipe, std::memory_order_release);
            connected_.store(true, std::memory_order_release);
            connection_handler_(true);
            static_cast<void>(send(glance::contracts::MessageType::hello));

            while (!stopping_.load(std::memory_order_acquire))
            {
                glance::contracts::FrameHeader header;
                if (!read_exact(pipe, &header, sizeof(header)) || !glance::contracts::valid_header(header))
                {
                    break;
                }
                std::string payload(header.payload_size, '\0');
                if (header.payload_size > 0 && !read_exact(pipe, payload.data(), header.payload_size))
                {
                    break;
                }
                message_handler_(header.type, header.flags, std::move(payload));
            }

            connected_.store(false, std::memory_order_release);
            connection_handler_(false);
            {
                std::scoped_lock lock(write_mutex_);
                if (pipe_.exchange(nullptr, std::memory_order_acq_rel) == pipe)
                {
                    CloseHandle(pipe);
                }
            }
        }
    }
}

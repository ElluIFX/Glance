#pragma once

#include "glance/contracts/ipc_protocol.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace glance::core
{
    class PipeServer
    {
    public:
        using MessageHandler = std::function<void(
            glance::contracts::MessageType,
            std::uint32_t,
            std::string_view)>;
        using ConnectionHandler = std::function<void(bool)>;

        PipeServer(MessageHandler message_handler, ConnectionHandler connection_handler);
        ~PipeServer();

        PipeServer(const PipeServer&) = delete;
        PipeServer& operator=(const PipeServer&) = delete;

        [[nodiscard]] bool start();
        void stop() noexcept;
        [[nodiscard]] bool send(
            glance::contracts::MessageType type,
            std::string_view payload = {},
            std::uint32_t flags = 0);
        [[nodiscard]] bool connected() const noexcept { return connected_.load(std::memory_order_acquire); }
        [[nodiscard]] DWORD peer_process_id() const noexcept
        {
            return peer_process_id_.load(std::memory_order_acquire);
        }

    private:
        void run() noexcept;
        [[nodiscard]] bool read_exact(HANDLE pipe, void* buffer, DWORD size) noexcept;
        [[nodiscard]] bool write_exact(HANDLE pipe, const void* buffer, DWORD size) noexcept;

        MessageHandler message_handler_;
        ConnectionHandler connection_handler_;
        std::thread thread_;
        std::atomic_bool stopping_{};
        std::atomic_bool connected_{};
        std::atomic<HANDLE> pipe_{};
        std::atomic<DWORD> peer_process_id_{};
        std::mutex write_mutex_;
        std::atomic_uint64_t correlation_id_{};
    };
}

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

namespace glance::app
{
    class PipeClient
    {
    public:
        using MessageHandler = std::function<void(
            glance::contracts::MessageType,
            std::uint32_t,
            std::string)>;
        using ConnectionHandler = std::function<void(bool)>;

        PipeClient(MessageHandler message_handler, ConnectionHandler connection_handler);
        ~PipeClient();

        PipeClient(const PipeClient&) = delete;
        PipeClient& operator=(const PipeClient&) = delete;

        [[nodiscard]] bool start();
        void stop() noexcept;
        [[nodiscard]] bool send(
            glance::contracts::MessageType type,
            std::string_view payload = {},
            std::uint32_t flags = 0);

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
        std::mutex write_mutex_;
        std::atomic_uint64_t correlation_id_{};
    };
}


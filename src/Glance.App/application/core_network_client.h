#pragma once

#include "glance/contracts/ipc_protocol.h"
#include "glance/contracts/network_protocol.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace glance::app
{
    class CoreNetworkClient
    {
    public:
        using SendCallback = std::function<bool(
            glance::contracts::MessageType,
            std::string_view)>;
        using AutomaticUpdateCallback =
            std::function<void(std::optional<glance::contracts::UpdateCheckResult>)>;
        using DownloadProgressCallback =
            std::function<void(std::uint64_t downloaded, std::uint64_t total)>;

        CoreNetworkClient(SendCallback send, AutomaticUpdateCallback automatic_update);

        [[nodiscard]] glance::contracts::UpdateCheckResult check_for_updates(
            std::wstring_view current_version);
        [[nodiscard]] bool request_automatic_update_check(
            std::wstring_view current_version,
            std::uint64_t last_successful_check);
        void forget_automatic_update_requests() noexcept;
        [[nodiscard]] glance::contracts::NetworkDownloadResult download(
            const glance::contracts::NetworkDownloadRequest& request,
            const std::atomic_bool& cancelled,
            const DownloadProgressCallback& progress);
        [[nodiscard]] bool handle_message(
            glance::contracts::MessageType type,
            std::string_view payload) noexcept;
        void disconnect() noexcept;

    private:
        struct UpdateWaitState
        {
            std::condition_variable condition;
            bool completed{};
            glance::contracts::UpdateCheckResult result;
        };

        struct DownloadWaitState
        {
            std::condition_variable condition;
            bool completed{};
            glance::contracts::NetworkDownloadResult result;
            DownloadProgressCallback progress;
        };

        [[nodiscard]] std::uint64_t next_request_id() noexcept;

        SendCallback send_;
        AutomaticUpdateCallback automatic_update_;
        std::atomic_uint64_t next_request_id_{};
        std::mutex mutex_;
        std::unordered_map<std::uint64_t, std::shared_ptr<UpdateWaitState>> update_waiters_;
        std::unordered_set<std::uint64_t> automatic_update_requests_;
        std::unordered_map<std::uint64_t, std::shared_ptr<DownloadWaitState>> download_waiters_;
    };
}

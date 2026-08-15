#pragma once

#include "glance/contracts/ipc_protocol.h"
#include "glance/contracts/network_protocol.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

namespace glance::core
{
    struct FileDownloadRequest
    {
        std::wstring url;
        std::filesystem::path destination_path;
        std::wstring sha256;
        std::uint64_t expected_size{};
        std::uint64_t maximum_size{};
    };

    using FileDownloadProgressCallback =
        std::function<void(std::uint64_t downloaded, std::uint64_t total)>;

    [[nodiscard]] glance::contracts::UpdateCheckResult check_for_updates(
        std::wstring_view current_version) noexcept;
    [[nodiscard]] glance::contracts::NetworkDownloadResult download_file(
        const FileDownloadRequest& request,
        const std::atomic_bool& cancelled,
        const FileDownloadProgressCallback& progress) noexcept;

    class NetworkService
    {
    public:
        using SendCallback = std::function<bool(
            glance::contracts::MessageType,
            std::string_view)>;
        using PreviewActiveCallback = std::function<bool()>;

        NetworkService(SendCallback send, PreviewActiveCallback preview_active);
        ~NetworkService();

        NetworkService(const NetworkService&) = delete;
        NetworkService& operator=(const NetworkService&) = delete;

        void stop() noexcept;
        void request_update_check(glance::contracts::UpdateCheckRequest request);
        void request_download(glance::contracts::NetworkDownloadMessage request);
        void cancel_download(std::uint64_t request_id) noexcept;

    private:
        struct DownloadJob
        {
            glance::contracts::NetworkDownloadMessage message;
            std::shared_ptr<std::atomic_bool> cancelled;
        };

        void run_update_worker() noexcept;
        void run_download_worker() noexcept;
        void send_update_response(
            std::uint64_t request_id,
            const glance::contracts::UpdateCheckResult& result) noexcept;
        void send_update_deferred(std::uint64_t request_id) noexcept;

        SendCallback send_;
        PreviewActiveCallback preview_active_;
        std::atomic_bool stopping_{};
        std::mutex update_mutex_;
        std::condition_variable update_condition_;
        std::deque<glance::contracts::UpdateCheckRequest> update_requests_;
        std::optional<glance::contracts::UpdateCheckResult> cached_update_result_;
        std::thread update_worker_;
        std::mutex download_mutex_;
        std::condition_variable download_condition_;
        std::deque<DownloadJob> download_requests_;
        std::shared_ptr<std::atomic_bool> active_download_cancellation_;
        std::uint64_t active_download_request_id_{};
        std::thread download_worker_;
    };
}

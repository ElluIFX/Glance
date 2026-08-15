#include "network_service.h"
#include "glance/contracts/diagnostics.h"

#include <windows.h>
#include <roapi.h>

#include <chrono>
#include <utility>
#include <vector>

namespace
{
    std::uint64_t unix_time_now() noexcept
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }

    std::optional<std::filesystem::path> download_destination(
        const glance::contracts::NetworkDownloadRequest& request) noexcept
    {
        try
        {
            const std::filesystem::path file_name(request.file_name);
            if (request.file_name.empty() || file_name != file_name.filename() ||
                file_name == L"." || file_name == L".." || request.sha256.size() != 64 ||
                request.expected_size == 0 ||
                request.expected_size > glance::contracts::maximum_network_download_bytes)
            {
                return std::nullopt;
            }
            return std::filesystem::temp_directory_path() / L"Glance" / L"Downloads" /
                request.sha256 / file_name;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    class RuntimeApartment
    {
    public:
        RuntimeApartment() noexcept : result_(RoInitialize(RO_INIT_MULTITHREADED))
        {
        }

        ~RuntimeApartment()
        {
            if (SUCCEEDED(result_))
            {
                RoUninitialize();
            }
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return SUCCEEDED(result_);
        }

    private:
        HRESULT result_{};
    };
}

namespace glance::core
{
    NetworkService::NetworkService(
        SendCallback send,
        PreviewActiveCallback preview_active)
        : send_(std::move(send)),
          preview_active_(std::move(preview_active)),
          update_worker_([this] { run_update_worker(); }),
          download_worker_([this] { run_download_worker(); })
    {
    }

    NetworkService::~NetworkService()
    {
        stop();
    }

    void NetworkService::stop() noexcept
    {
        if (stopping_.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }
        {
            std::scoped_lock lock(download_mutex_);
            if (active_download_cancellation_)
            {
                active_download_cancellation_->store(true, std::memory_order_release);
            }
            for (const auto& job : download_requests_)
            {
                job.cancelled->store(true, std::memory_order_release);
            }
        }
        update_condition_.notify_all();
        download_condition_.notify_all();
        if (update_worker_.joinable())
        {
            update_worker_.join();
        }
        if (download_worker_.joinable())
        {
            download_worker_.join();
        }
    }

    void NetworkService::request_update_check(
        glance::contracts::UpdateCheckRequest request)
    {
        std::optional<glance::contracts::UpdateCheckResult> cached;
        {
            std::scoped_lock lock(update_mutex_);
            if (request.automatic && cached_update_result_ &&
                glance::contracts::cached_update_result_is_newer(
                    cached_update_result_->checked_at,
                    request.last_successful_check))
            {
                cached = cached_update_result_;
            }
            else
            {
                update_requests_.push_back(std::move(request));
            }
        }
        if (cached)
        {
            if (!request.automatic ||
                (preview_active_ && preview_active_()))
            {
                send_update_response(request.request_id, *cached);
            }
            else
            {
                send_update_deferred(request.request_id);
            }
        }
        else
        {
            update_condition_.notify_one();
        }
    }

    void NetworkService::request_download(
        glance::contracts::NetworkDownloadMessage request)
    {
        auto cancellation = std::make_shared<std::atomic_bool>(false);
        {
            std::scoped_lock lock(download_mutex_);
            download_requests_.push_back(
                DownloadJob{ std::move(request), std::move(cancellation) });
        }
        download_condition_.notify_one();
    }

    void NetworkService::cancel_download(std::uint64_t request_id) noexcept
    {
        std::scoped_lock lock(download_mutex_);
        if (active_download_request_id_ == request_id && active_download_cancellation_)
        {
            active_download_cancellation_->store(true, std::memory_order_release);
        }
        for (const auto& job : download_requests_)
        {
            if (job.message.request_id == request_id)
            {
                job.cancelled->store(true, std::memory_order_release);
            }
        }
    }

    void NetworkService::run_update_worker() noexcept
    {
        const RuntimeApartment apartment;
        if (!apartment)
        {
            glance::contracts::log_event(L"Failed to initialize the Core update worker.");
        }
        while (!stopping_.load(std::memory_order_acquire))
        {
            glance::contracts::UpdateCheckRequest request;
            {
                std::unique_lock lock(update_mutex_);
                update_condition_.wait(lock, [this] {
                    return stopping_.load(std::memory_order_acquire) ||
                        !update_requests_.empty();
                });
                if (stopping_.load(std::memory_order_acquire))
                {
                    return;
                }
                request = std::move(update_requests_.front());
                update_requests_.pop_front();
            }

            auto result = apartment
                ? check_for_updates(request.current_version)
                : glance::contracts::UpdateCheckResult{};
            if (glance::contracts::update_check_succeeded(result.status))
            {
                result.checked_at = unix_time_now();
                std::scoped_lock lock(update_mutex_);
                cached_update_result_ = result;
            }
            if (!request.automatic ||
                glance::contracts::should_publish_automatic_update_result(
                    result.status,
                    preview_active_ && preview_active_()))
            {
                send_update_response(request.request_id, result);
            }
            else
            {
                send_update_deferred(request.request_id);
            }
        }
    }

    void NetworkService::run_download_worker() noexcept
    {
        while (!stopping_.load(std::memory_order_acquire))
        {
            DownloadJob job;
            {
                std::unique_lock lock(download_mutex_);
                download_condition_.wait(lock, [this] {
                    return stopping_.load(std::memory_order_acquire) ||
                        !download_requests_.empty();
                });
                if (stopping_.load(std::memory_order_acquire))
                {
                    return;
                }
                job = std::move(download_requests_.front());
                download_requests_.pop_front();
                active_download_request_id_ = job.message.request_id;
                active_download_cancellation_ = job.cancelled;
            }

            glance::contracts::NetworkDownloadResult result{
                glance::contracts::NetworkDownloadStatus::integrity_error, {} };
            if (const auto destination = download_destination(job.message.request))
            {
                result = download_file(
                    FileDownloadRequest{
                        .url = job.message.request.url,
                        .destination_path = *destination,
                        .sha256 = job.message.request.sha256,
                        .expected_size = job.message.request.expected_size,
                        .maximum_size = glance::contracts::maximum_network_download_bytes },
                    *job.cancelled,
                    [this, request_id = job.message.request_id](
                        std::uint64_t downloaded,
                        std::uint64_t total) {
                        const auto payload = glance::contracts::encode_network_download_progress(
                            { request_id, downloaded, total });
                        static_cast<void>(send_(
                            glance::contracts::MessageType::network_download_progress,
                            payload));
                    });
            }

            try
            {
                const auto payload = glance::contracts::encode_network_download_response(
                    { job.message.request_id, std::move(result) });
                static_cast<void>(send_(
                    glance::contracts::MessageType::network_download_response,
                    payload));
            }
            catch (...)
            {
                glance::contracts::log_event(L"Failed to encode a Core download response.");
            }
            {
                std::scoped_lock lock(download_mutex_);
                if (active_download_request_id_ == job.message.request_id)
                {
                    active_download_request_id_ = 0;
                    active_download_cancellation_.reset();
                }
            }
        }
    }

    void NetworkService::send_update_response(
        std::uint64_t request_id,
        const glance::contracts::UpdateCheckResult& result) noexcept
    {
        try
        {
            const auto payload = glance::contracts::encode_update_check_response(
                { request_id, result });
            static_cast<void>(send_(
                glance::contracts::MessageType::update_check_response,
                payload));
        }
        catch (...)
        {
            glance::contracts::log_event(L"Failed to encode a Core update response.");
        }
    }

    void NetworkService::send_update_deferred(std::uint64_t request_id) noexcept
    {
        try
        {
            const auto payload = glance::contracts::encode_update_check_deferred(request_id);
            static_cast<void>(send_(
                glance::contracts::MessageType::update_check_deferred,
                payload));
        }
        catch (...)
        {
            glance::contracts::log_event(L"Failed to encode a deferred update response.");
        }
    }
}

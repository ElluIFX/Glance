#include "pch.h"
#include "core_network_client.h"

#include <chrono>
#include <utility>
#include <vector>

namespace glance::app
{
    CoreNetworkClient::CoreNetworkClient(
        SendCallback send,
        AutomaticUpdateCallback automatic_update)
        : send_(std::move(send)), automatic_update_(std::move(automatic_update))
    {
    }

    glance::contracts::UpdateCheckResult CoreNetworkClient::check_for_updates(
        std::wstring_view current_version)
    {
        const auto request_id = next_request_id();
        const auto waiter = std::make_shared<UpdateWaitState>();
        {
            std::scoped_lock lock(mutex_);
            update_waiters_.emplace(request_id, waiter);
        }

        try
        {
            const auto payload = glance::contracts::encode_update_check_request(
                { request_id, 0, false, std::wstring(current_version) });
            if (!send_(glance::contracts::MessageType::update_check_request, payload))
            {
                std::scoped_lock lock(mutex_);
                update_waiters_.erase(request_id);
                return {};
            }
        }
        catch (...)
        {
            std::scoped_lock lock(mutex_);
            update_waiters_.erase(request_id);
            return {};
        }

        std::unique_lock lock(mutex_);
        if (!waiter->condition.wait_for(lock, std::chrono::seconds(30), [&waiter] {
                return waiter->completed;
            }))
        {
            update_waiters_.erase(request_id);
            return {};
        }
        update_waiters_.erase(request_id);
        return waiter->result;
    }

    bool CoreNetworkClient::request_automatic_update_check(
        std::wstring_view current_version,
        std::uint64_t last_successful_check)
    {
        const auto request_id = next_request_id();
        {
            std::scoped_lock lock(mutex_);
            automatic_update_requests_.insert(request_id);
        }
        try
        {
            const auto payload = glance::contracts::encode_update_check_request(
                { request_id, last_successful_check, true, std::wstring(current_version) });
            if (send_(glance::contracts::MessageType::update_check_request, payload))
            {
                return true;
            }
        }
        catch (...)
        {
        }
        std::scoped_lock lock(mutex_);
        automatic_update_requests_.erase(request_id);
        return false;
    }

    void CoreNetworkClient::forget_automatic_update_requests() noexcept
    {
        std::scoped_lock lock(mutex_);
        automatic_update_requests_.clear();
    }

    glance::contracts::NetworkDownloadResult CoreNetworkClient::download(
        const glance::contracts::NetworkDownloadRequest& request,
        const std::atomic_bool& cancelled,
        const DownloadProgressCallback& progress)
    {
        const auto request_id = next_request_id();
        const auto waiter = std::make_shared<DownloadWaitState>();
        waiter->progress = progress;
        {
            std::scoped_lock lock(mutex_);
            download_waiters_.emplace(request_id, waiter);
        }

        try
        {
            const auto payload = glance::contracts::encode_network_download_request(
                { request_id, request });
            if (!send_(glance::contracts::MessageType::network_download_request, payload))
            {
                std::scoped_lock lock(mutex_);
                download_waiters_.erase(request_id);
                return {};
            }
        }
        catch (...)
        {
            std::scoped_lock lock(mutex_);
            download_waiters_.erase(request_id);
            return { glance::contracts::NetworkDownloadStatus::integrity_error, {} };
        }

        bool cancel_sent{};
        std::unique_lock lock(mutex_);
        while (!waiter->completed)
        {
            static_cast<void>(waiter->condition.wait_for(lock, std::chrono::milliseconds(100)));
            if (!cancel_sent && cancelled.load(std::memory_order_acquire))
            {
                cancel_sent = true;
                lock.unlock();
                try
                {
                    const auto payload =
                        glance::contracts::encode_network_download_cancel(request_id);
                    static_cast<void>(send_(
                        glance::contracts::MessageType::network_download_cancel,
                        payload));
                }
                catch (...)
                {
                }
                lock.lock();
            }
        }
        download_waiters_.erase(request_id);
        return waiter->result;
    }

    bool CoreNetworkClient::handle_message(
        glance::contracts::MessageType type,
        std::string_view payload) noexcept
    {
        if (type == glance::contracts::MessageType::update_check_response)
        {
            const auto response = glance::contracts::decode_update_check_response(payload);
            if (!response)
            {
                return true;
            }

            std::shared_ptr<UpdateWaitState> waiter;
            bool automatic{};
            {
                std::scoped_lock lock(mutex_);
                if (const auto found = update_waiters_.find(response->request_id);
                    found != update_waiters_.end())
                {
                    waiter = found->second;
                    waiter->result = response->result;
                    waiter->completed = true;
                }
                else
                {
                    automatic = automatic_update_requests_.erase(response->request_id) != 0;
                }
            }
            if (waiter)
            {
                waiter->condition.notify_one();
            }
            else if (automatic && automatic_update_)
            {
                try
                {
                    automatic_update_(response->result);
                }
                catch (...)
                {
                }
            }
            return true;
        }

        if (type == glance::contracts::MessageType::update_check_deferred)
        {
            const auto request_id = glance::contracts::decode_update_check_deferred(payload);
            if (!request_id)
            {
                return true;
            }
            bool automatic{};
            {
                std::scoped_lock lock(mutex_);
                automatic = automatic_update_requests_.erase(*request_id) != 0;
            }
            if (automatic && automatic_update_)
            {
                try
                {
                    automatic_update_(std::nullopt);
                }
                catch (...)
                {
                }
            }
            return true;
        }

        if (type == glance::contracts::MessageType::network_download_progress)
        {
            const auto response = glance::contracts::decode_network_download_progress(payload);
            if (!response)
            {
                return true;
            }
            DownloadProgressCallback progress;
            {
                std::scoped_lock lock(mutex_);
                if (const auto found = download_waiters_.find(response->request_id);
                    found != download_waiters_.end())
                {
                    progress = found->second->progress;
                }
            }
            if (progress)
            {
                try
                {
                    progress(response->downloaded, response->total);
                }
                catch (...)
                {
                }
            }
            return true;
        }

        if (type == glance::contracts::MessageType::network_download_response)
        {
            const auto response = glance::contracts::decode_network_download_response(payload);
            if (!response)
            {
                return true;
            }
            std::shared_ptr<DownloadWaitState> waiter;
            {
                std::scoped_lock lock(mutex_);
                if (const auto found = download_waiters_.find(response->request_id);
                    found != download_waiters_.end())
                {
                    waiter = found->second;
                    waiter->result = response->result;
                    waiter->completed = true;
                }
            }
            if (waiter)
            {
                waiter->condition.notify_one();
            }
            return true;
        }
        return false;
    }

    void CoreNetworkClient::disconnect() noexcept
    {
        std::vector<std::shared_ptr<UpdateWaitState>> update_waiters;
        std::vector<std::shared_ptr<DownloadWaitState>> download_waiters;
        {
            std::scoped_lock lock(mutex_);
            for (const auto& [request_id, waiter] : update_waiters_)
            {
                static_cast<void>(request_id);
                waiter->completed = true;
                update_waiters.push_back(waiter);
            }
            for (const auto& [request_id, waiter] : download_waiters_)
            {
                static_cast<void>(request_id);
                waiter->completed = true;
                download_waiters.push_back(waiter);
            }
            automatic_update_requests_.clear();
        }
        for (const auto& waiter : update_waiters)
        {
            waiter->condition.notify_one();
        }
        for (const auto& waiter : download_waiters)
        {
            waiter->condition.notify_one();
        }
    }

    std::uint64_t CoreNetworkClient::next_request_id() noexcept
    {
        auto result = next_request_id_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (result == 0)
        {
            result = next_request_id_.fetch_add(1, std::memory_order_relaxed) + 1;
        }
        return result;
    }
}

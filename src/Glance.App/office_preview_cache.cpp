#include "pch.h"
#include "office_preview_cache.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <list>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;
    using Entry = glance::app::OfficePreviewCacheHandle;

    struct CachedEntry
    {
        Entry entry;
        Clock::time_point last_access;
    };

    bool same_path(const std::wstring& left, const std::wstring& right) noexcept
    {
        return _wcsicmp(left.c_str(), right.c_str()) == 0;
    }

    class OfficePreviewCache final
    {
    public:
        OfficePreviewCache()
            : worker_([this] { cleanup_worker(); })
        {
        }

        ~OfficePreviewCache()
        {
            shutdown();
        }

        void configure(const glance::app::OfficePreviewPreferences& preferences) noexcept
        {
            try
            {
                std::scoped_lock lock(mutex_);
                capacity_ = std::min<std::uint32_t>(preferences.cache_capacity, 16);
                expiration_ = std::chrono::minutes(std::clamp<std::uint32_t>(
                    preferences.cache_expiration_minutes,
                    1,
                    60));
                prune_expired_locked(Clock::now());
                trim_capacity_locked();
                ++revision_;
                condition_.notify_one();
            }
            catch (...)
            {
            }
        }

        Entry take(
            const std::wstring& source_path,
            std::uint64_t source_size,
            std::uint64_t source_modified_time) noexcept
        {
            try
            {
                std::scoped_lock lock(mutex_);
                if (stopping_)
                {
                    return {};
                }
                prune_expired_locked(Clock::now());
                for (auto iterator = entries_.begin(); iterator != entries_.end(); ++iterator)
                {
                    const auto& entry = iterator->entry;
                    if (!same_path(entry->source_path, source_path) ||
                        entry->source_size != source_size ||
                        entry->source_modified_time != source_modified_time ||
                        !entry->ready.load(std::memory_order_acquire))
                    {
                        continue;
                    }
                    if (entry->kind == glance::app::OfficePreviewCacheKind::pdf)
                    {
                        std::error_code error;
                        if (!std::filesystem::is_regular_file(entry->pdf_path, error))
                        {
                            retire_locked(std::move(iterator->entry));
                            entries_.erase(iterator);
                            ++revision_;
                            condition_.notify_one();
                            return {};
                        }
                    }
                    auto result = std::move(iterator->entry);
                    entries_.erase(iterator);
                    ++revision_;
                    condition_.notify_one();
                    return result;
                }
            }
            catch (...)
            {
            }
            return {};
        }

        void put(Entry entry) noexcept
        {
            if (entry == nullptr)
            {
                return;
            }
            try
            {
                std::scoped_lock lock(mutex_);
                if (stopping_ ||
                    capacity_ == 0 ||
                    !entry->ready.load(std::memory_order_acquire))
                {
                    retire_locked(std::move(entry));
                    ++revision_;
                    condition_.notify_one();
                    return;
                }

                for (auto iterator = entries_.begin(); iterator != entries_.end();)
                {
                    if (same_path(iterator->entry->source_path, entry->source_path))
                    {
                        retire_locked(std::move(iterator->entry));
                        iterator = entries_.erase(iterator);
                    }
                    else
                    {
                        ++iterator;
                    }
                }
                entries_.push_front({ std::move(entry), Clock::now() });
                trim_capacity_locked();
                ++revision_;
                condition_.notify_one();
            }
            catch (...)
            {
            }
        }

        void shutdown() noexcept
        {
            {
                std::scoped_lock lock(mutex_);
                if (stopping_)
                {
                    return;
                }
                stopping_ = true;
                for (auto& cached : entries_)
                {
                    retire_locked(std::move(cached.entry));
                }
                entries_.clear();
                ++revision_;
            }
            condition_.notify_one();
            if (worker_.joinable())
            {
                worker_.join();
            }
        }

    private:
        void retire_locked(Entry entry)
        {
            if (entry != nullptr)
            {
                retired_.push_back(std::move(entry));
            }
        }

        void trim_capacity_locked()
        {
            while (entries_.size() > capacity_)
            {
                retire_locked(std::move(entries_.back().entry));
                entries_.pop_back();
            }
        }

        void prune_expired_locked(Clock::time_point now)
        {
            for (auto iterator = entries_.begin(); iterator != entries_.end();)
            {
                if (now - iterator->last_access >= expiration_)
                {
                    retire_locked(std::move(iterator->entry));
                    iterator = entries_.erase(iterator);
                }
                else
                {
                    ++iterator;
                }
            }
        }

        Clock::time_point next_expiration_locked() const
        {
            Clock::time_point result = Clock::time_point::max();
            for (const auto& cached : entries_)
            {
                result = std::min(result, cached.last_access + expiration_);
            }
            return result;
        }

        void cleanup_worker()
        {
            std::vector<Entry> cleanup;
            std::unique_lock lock(mutex_);
            while (true)
            {
                prune_expired_locked(Clock::now());
                cleanup.swap(retired_);
                if (!cleanup.empty())
                {
                    lock.unlock();
                    cleanup.clear();
                    lock.lock();
                    continue;
                }
                if (stopping_)
                {
                    break;
                }

                const auto revision = revision_;
                const auto deadline = next_expiration_locked();
                if (deadline == Clock::time_point::max())
                {
                    condition_.wait(lock, [this, revision] {
                        return stopping_ || revision_ != revision;
                    });
                }
                else
                {
                    condition_.wait_until(lock, deadline, [this, revision] {
                        return stopping_ || revision_ != revision;
                    });
                }
            }
            cleanup.swap(retired_);
            lock.unlock();
            cleanup.clear();
        }

        std::mutex mutex_;
        std::condition_variable condition_;
        std::list<CachedEntry> entries_;
        std::vector<Entry> retired_;
        std::uint32_t capacity_{ 1 };
        std::chrono::minutes expiration_{ 5 };
        std::uint64_t revision_{};
        bool stopping_{};
        std::thread worker_;
    };

    OfficePreviewCache& cache()
    {
        static OfficePreviewCache instance;
        return instance;
    }
}

namespace glance::app
{
    OfficePreviewCacheEntry::~OfficePreviewCacheEntry()
    {
        word_client.reset();
        if (!pdf_path.empty())
        {
            std::error_code error;
            std::filesystem::remove(pdf_path, error);
        }
    }

    void configure_office_preview_cache(const OfficePreviewPreferences& preferences) noexcept
    {
        cache().configure(preferences);
    }

    OfficePreviewCacheHandle take_office_preview_cache(
        const std::wstring& source_path,
        std::uint64_t source_size,
        std::uint64_t source_modified_time) noexcept
    {
        return cache().take(source_path, source_size, source_modified_time);
    }

    void return_office_preview_cache(OfficePreviewCacheHandle entry) noexcept
    {
        cache().put(std::move(entry));
    }

    void shutdown_office_preview_cache() noexcept
    {
        cache().shutdown();
    }
}

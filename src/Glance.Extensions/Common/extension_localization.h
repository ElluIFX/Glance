#pragma once

#include <windows.h>
#include <mrm.h>

#include <cstddef>
#include <mutex>
#include <string_view>

namespace glance::extensions
{
    class ResourceStore
    {
    public:
        ResourceStore() = default;
        ResourceStore(const ResourceStore&) = delete;
        ResourceStore& operator=(const ResourceStore&) = delete;
        ~ResourceStore();

        [[nodiscard]] bool initialize() noexcept;
        void shutdown() noexcept;
        [[nodiscard]] bool copy(
            std::wstring_view key,
            const wchar_t* language_tag,
            wchar_t* destination,
            std::size_t capacity) noexcept;

    private:
        std::mutex mutex_;
        MrmManagerHandle manager_{};
        MrmContextHandle context_{};
        MrmMapHandle resources_{};
    };
}

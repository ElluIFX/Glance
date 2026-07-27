#pragma once

#include <mrm.h>
#include <windows.h>

#include <cstddef>
#include <mutex>
#include <string_view>

namespace glance::components
{
    class ComponentResourceStore
    {
    public:
        ComponentResourceStore() = default;
        ComponentResourceStore(const ComponentResourceStore&) = delete;
        ComponentResourceStore& operator=(const ComponentResourceStore&) = delete;
        ~ComponentResourceStore();

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

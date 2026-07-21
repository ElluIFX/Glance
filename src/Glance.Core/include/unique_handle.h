#pragma once

#include <windows.h>

#include <utility>

namespace glance::core
{
    class unique_handle
    {
    public:
        unique_handle() noexcept = default;
        explicit unique_handle(HANDLE handle) noexcept : handle_(handle) {}
        ~unique_handle() { reset(); }

        unique_handle(const unique_handle&) = delete;
        unique_handle& operator=(const unique_handle&) = delete;

        unique_handle(unique_handle&& other) noexcept : handle_(other.release()) {}

        unique_handle& operator=(unique_handle&& other) noexcept
        {
            if (this != &other)
            {
                reset(other.release());
            }
            return *this;
        }

        [[nodiscard]] HANDLE get() const noexcept { return handle_; }
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
        }

        [[nodiscard]] HANDLE release() noexcept
        {
            return std::exchange(handle_, nullptr);
        }

        void reset(HANDLE handle = nullptr) noexcept
        {
            if (*this)
            {
                CloseHandle(handle_);
            }
            handle_ = handle;
        }

    private:
        HANDLE handle_{};
    };
}


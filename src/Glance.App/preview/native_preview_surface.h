#pragma once

#include "glance/contracts/native_preview_protocol.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>

namespace glance::app
{
    class NativePreviewSurface final
    {
    public:
        using DoubleClickCallback = std::function<void()>;

        NativePreviewSurface(
            HWND parent,
            std::wstring host_path,
            std::shared_ptr<void> renderer_lease,
            DoubleClickCallback double_click_callback);
        ~NativePreviewSurface();

        NativePreviewSurface(const NativePreviewSurface&) = delete;
        NativePreviewSurface& operator=(const NativePreviewSurface&) = delete;

        [[nodiscard]] bool available() const noexcept;
        [[nodiscard]] bool start_host() noexcept;
        [[nodiscard]] glance::contracts::native_preview::Status open(
            const std::wstring& path,
            const glance::contracts::native_preview::PreviewVisuals& visuals,
            std::uint32_t dpi);
        void resize(std::uint32_t width, std::uint32_t height, std::uint32_t dpi) noexcept;
        void set_visuals(
            const glance::contracts::native_preview::PreviewVisuals& visuals) noexcept;
        void set_bounds(int x, int y, int width, int height) noexcept;
        void set_occlusions(std::span<const RECT> rectangles) noexcept;
        void set_visible(bool visible) noexcept;
        void set_double_click_enabled(bool enabled) noexcept;
        void cancel() noexcept;
        void destroy_surface() noexcept;
        void shutdown() noexcept;
        [[nodiscard]] bool handle_mouse_message(
            WPARAM message,
            const POINT& point) noexcept;

    public:
        static LRESULT CALLBACK host_window_proc(
            HWND window,
            UINT message,
            WPARAM wparam,
            LPARAM lparam) noexcept;

    private:
        [[nodiscard]] bool start_process_locked();
        [[nodiscard]] bool transact_locked(
            glance::contracts::native_preview::Command command,
            const void* payload,
            std::uint32_t payload_size,
            glance::contracts::native_preview::Status& status,
            DWORD timeout_ms) noexcept;
        void close_process_locked(bool terminate) noexcept;
        void update_mouse_hook_registration() noexcept;

        HWND parent_{};
        HWND host_{};
        HWND content_{};
        DWORD surface_thread_id_{};
        std::wstring host_path_;
        std::shared_ptr<void> renderer_lease_;
        DoubleClickCallback double_click_callback_;
        std::mutex io_mutex_;
        std::mutex process_mutex_;
        std::atomic_bool cancelled_{};
        HANDLE process_{};
        HANDLE request_pipe_{};
        HANDLE response_pipe_{};
        HANDLE cancellation_event_{};
        std::atomic<COLORREF> background_{ RGB(32, 32, 32) };
        std::uint32_t width_{};
        std::uint32_t height_{};
        bool visible_{};
        bool double_click_enabled_{};
        bool mouse_hook_registered_{};
        bool suppress_next_left_up_{};
        ULONGLONG last_left_down_tick_{};
        POINT last_left_down_point_{};
    };
}

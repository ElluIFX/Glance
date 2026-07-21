#pragma once

#include <windows.h>
#include <shellapi.h>

#include <functional>

namespace glance::app
{
    class TrayIcon
    {
    public:
        using Callback = std::function<void()>;

        TrayIcon() = default;
        ~TrayIcon();

        TrayIcon(const TrayIcon&) = delete;
        TrayIcon& operator=(const TrayIcon&) = delete;

        [[nodiscard]] bool create(HINSTANCE instance, Callback settings_callback, Callback exit_callback);
        void remove() noexcept;

    private:
        static constexpr UINT callback_message = WM_APP + 20;
        static constexpr UINT settings_command = 1;
        static constexpr UINT exit_command = 2;

        static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
        void show_menu();
        [[nodiscard]] bool add_icon();

        HWND window_{};
        HINSTANCE instance_{};
        NOTIFYICONDATAW icon_{};
        UINT taskbar_created_message_{};
        Callback settings_callback_;
        Callback exit_callback_;
    };
}

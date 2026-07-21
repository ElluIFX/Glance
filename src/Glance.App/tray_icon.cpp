#include "pch.h"
#include "tray_icon.h"
#include "resource.h"

namespace glance::app
{
    TrayIcon::~TrayIcon()
    {
        remove();
    }

    bool TrayIcon::create(HINSTANCE instance, Callback settings_callback, Callback exit_callback)
    {
        instance_ = instance;
        settings_callback_ = std::move(settings_callback);
        exit_callback_ = std::move(exit_callback);
        taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");

        constexpr wchar_t class_name[] = L"Glance.TrayIconWindow";
        WNDCLASSEXW window_class{ sizeof(WNDCLASSEXW) };
        window_class.hInstance = instance;
        window_class.lpfnWndProc = window_proc;
        window_class.lpszClassName = class_name;
        if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return false;
        }
        window_ = CreateWindowExW(
            WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            class_name,
            L"",
            WS_POPUP,
            0,
            0,
            0,
            0,
            nullptr,
            nullptr,
            instance,
            this);
        if (window_ == nullptr)
        {
            return false;
        }
        return add_icon();
    }

    bool TrayIcon::add_icon()
    {
        icon_ = {};
        icon_.cbSize = sizeof(icon_);
        icon_.hWnd = window_;
        icon_.uID = 1;
        icon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        icon_.uCallbackMessage = callback_message;
        icon_.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_GLANCE_APP));
        if (icon_.hIcon == nullptr)
        {
            icon_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        }
        wcscpy_s(icon_.szTip, L"Glance");
        if (!Shell_NotifyIconW(NIM_ADD, &icon_))
        {
            return false;
        }
        icon_.uVersion = NOTIFYICON_VERSION_4;
        static_cast<void>(Shell_NotifyIconW(NIM_SETVERSION, &icon_));
        return true;
    }

    void TrayIcon::remove() noexcept
    {
        if (icon_.hWnd != nullptr)
        {
            Shell_NotifyIconW(NIM_DELETE, &icon_);
            icon_ = {};
        }
        if (window_ != nullptr)
        {
            DestroyWindow(window_);
            window_ = nullptr;
        }
    }

    LRESULT CALLBACK TrayIcon::window_proc(
        HWND window,
        UINT message,
        WPARAM wparam,
        LPARAM lparam) noexcept
    {
        TrayIcon* self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            self = static_cast<TrayIcon*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self == nullptr)
        {
            return DefWindowProcW(window, message, wparam, lparam);
        }
        if (message == self->taskbar_created_message_)
        {
            static_cast<void>(self->add_icon());
            return 0;
        }
        if (message == callback_message)
        {
            const UINT event = LOWORD(lparam);
            if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP)
            {
                self->show_menu();
                return 0;
            }
            if (event == WM_LBUTTONDBLCLK && self->settings_callback_)
            {
                self->settings_callback_();
                return 0;
            }
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void TrayIcon::show_menu()
    {
        POINT cursor{};
        GetCursorPos(&cursor);
        HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
        {
            return;
        }
        AppendMenuW(menu, MF_STRING, settings_command, L"Settings");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, exit_command, L"Exit Glance");
        SetForegroundWindow(window_);
        const UINT command = TrackPopupMenu(
            menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
            cursor.x,
            cursor.y,
            0,
            window_,
            nullptr);
        DestroyMenu(menu);
        if (command == settings_command && settings_callback_)
        {
            settings_callback_();
        }
        else if (command == exit_command && exit_callback_)
        {
            exit_callback_();
        }
    }
}

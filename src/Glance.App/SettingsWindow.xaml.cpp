#include "pch.h"
#include "SettingsWindow.xaml.h"
#if __has_include("SettingsWindow.g.cpp")
#include "SettingsWindow.g.cpp"
#endif

#include <microsoft.ui.xaml.window.h>

#include <filesystem>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace
{
    std::wstring executable_path()
    {
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        path.resize(length);
        return path;
    }
}

namespace winrt::Glance::App::implementation
{
    SettingsWindow::SettingsWindow()
    {
        InitializeComponent();
        Title(L"Glance Settings");
        HWND window{};
        check_hresult(this->try_as<::IWindowNative>()->get_WindowHandle(&window));
        SetWindowPos(window, nullptr, 0, 0, 520, 360, SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);

        initializing_ = true;
        LaunchAtSignInToggle().IsOn(launch_at_sign_in_enabled());
        initializing_ = false;
        refresh_core_status();
    }

    void SettingsWindow::InitializeSession(ExitCallback exit_callback)
    {
        exit_callback_ = std::move(exit_callback);
    }

    bool SettingsWindow::launch_at_sign_in_enabled() const
    {
        wchar_t value[32768]{};
        DWORD size = sizeof(value);
        return RegGetValueW(
                   HKEY_CURRENT_USER,
                   L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                   L"Glance",
                   RRF_RT_REG_SZ,
                   nullptr,
                   value,
                   &size) == ERROR_SUCCESS;
    }

    void SettingsWindow::set_launch_at_sign_in(bool enabled)
    {
        HKEY key{};
        if (RegCreateKeyExW(
                HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                0,
                nullptr,
                0,
                KEY_SET_VALUE,
                nullptr,
                &key,
                nullptr) != ERROR_SUCCESS)
        {
            return;
        }
        if (enabled)
        {
            const std::wstring command = L"\"" + executable_path() + L"\"";
            RegSetValueExW(
                key,
                L"Glance",
                0,
                REG_SZ,
                reinterpret_cast<const BYTE*>(command.c_str()),
                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        }
        else
        {
            RegDeleteValueW(key, L"Glance");
        }
        RegCloseKey(key);
    }

    void SettingsWindow::refresh_core_status()
    {
        HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\Glance.Core");
        CoreStatusText().Text(mutex != nullptr ? L"Running" : L"Not running");
        if (mutex != nullptr)
        {
            CloseHandle(mutex);
        }
    }

    void SettingsWindow::LaunchAtSignInToggle_Toggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (!initializing_)
        {
            set_launch_at_sign_in(LaunchAtSignInToggle().IsOn());
        }
    }

    void SettingsWindow::RefreshCoreStatusButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        refresh_core_status();
    }

    void SettingsWindow::ExitButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (exit_callback_)
        {
            exit_callback_();
        }
    }
}

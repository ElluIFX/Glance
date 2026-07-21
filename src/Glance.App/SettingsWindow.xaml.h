#pragma once

#include "SettingsWindow.g.h"

#include <functional>

namespace winrt::Glance::App::implementation
{
    struct SettingsWindow : SettingsWindowT<SettingsWindow>
    {
        using ExitCallback = std::function<void()>;

        SettingsWindow();
        void InitializeSession(ExitCallback exit_callback);

        void LaunchAtSignInToggle_Toggled(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RefreshCoreStatusButton_Click(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ExitButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        void refresh_core_status();
        [[nodiscard]] bool launch_at_sign_in_enabled() const;
        void set_launch_at_sign_in(bool enabled);

        bool initializing_{};
        ExitCallback exit_callback_;
    };
}

namespace winrt::Glance::App::factory_implementation
{
    struct SettingsWindow : SettingsWindowT<SettingsWindow, implementation::SettingsWindow> {};
}

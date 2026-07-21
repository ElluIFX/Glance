#pragma once

#include "SettingsWindow.g.h"
#include "appearance_preferences.h"
#include "path_copy_preferences.h"
#include "text_preferences.h"

#include <functional>

namespace winrt::Glance::App::implementation
{
    struct SettingsWindow : SettingsWindowT<SettingsWindow>
    {
        using ExitCallback = std::function<void()>;
        using AppearanceChangedCallback = std::function<void()>;
        using TextPreferencesChangedCallback = std::function<void()>;

        SettingsWindow();
        void InitializeSession(
            ExitCallback exit_callback,
            AppearanceChangedCallback appearance_changed_callback,
            TextPreferencesChangedCallback text_preferences_changed_callback);
        void ApplyAppearancePreferences();
        void ApplyLocalizedResources();
        winrt::fire_and_forget ConfirmExit();

        void LaunchAtSignInToggle_Toggled(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void DiagnosticsToggle_Toggled(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void AutoFitWindowSizeToggle_Toggled(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RefreshCoreStatusButton_Click(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ResetWindowSizesButton_Click(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void FontFamilyComboBox_SelectionChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void FontSizeNumberBox_ValueChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const&);
        void TextPreferenceToggle_Toggled(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void PathCopyPreferenceToggle_Toggled(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void AppearanceComboBox_SelectionChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void SettingsNavigation_SelectionChanged(
            Microsoft::UI::Xaml::Controls::NavigationView const&,
            Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const&);
        winrt::fire_and_forget ExitButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void CloseSettingsButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        void configure_window();
        void refresh_core_status();
        [[nodiscard]] bool launch_at_sign_in_enabled() const;
        void set_launch_at_sign_in(bool enabled);
        void save_text_preferences();
        void save_appearance_preferences();

        bool initializing_{};
        bool exit_confirmation_open_{};
        glance::app::TextPreferences text_preferences_{};
        glance::app::PathCopyPreferences path_copy_preferences_{};
        glance::app::AppearancePreferences appearance_preferences_{};
        ExitCallback exit_callback_;
        AppearanceChangedCallback appearance_changed_callback_;
        TextPreferencesChangedCallback text_preferences_changed_callback_;
    };
}

namespace winrt::Glance::App::factory_implementation
{
    struct SettingsWindow : SettingsWindowT<SettingsWindow, implementation::SettingsWindow> {};
}

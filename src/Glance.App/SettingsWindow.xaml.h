#pragma once

#include "SettingsWindow.g.h"
#include "appearance_preferences.h"
#include "footer_preferences.h"
#include "media_preview_preferences.h"
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
        using FooterPreferencesChangedCallback = std::function<void()>;

        SettingsWindow();
        void InitializeSession(
            ExitCallback exit_callback,
            AppearanceChangedCallback appearance_changed_callback,
            TextPreferencesChangedCallback text_preferences_changed_callback,
            FooterPreferencesChangedCallback footer_preferences_changed_callback);
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
        void DefaultAudioVolumeNumberBox_ValueChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const&);
        void DefaultVideoVolumeNumberBox_ValueChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const&);
        winrt::fire_and_forget ExportDiagnosticBundleButton_Click(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget ResetAllSettingsButton_Click(
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
        void SyntaxThemeComboBox_SelectionChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void TextPreferenceToggle_Toggled(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void PathCopyPreferenceToggle_Toggled(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void FooterFieldCheckBox_Click(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void FooterFieldMoveUpButton_Click(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void FooterFieldMoveDownButton_Click(
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
        enum class DiagnosticBundleState
        {
            idle,
            packaging,
            succeeded,
            failed,
        };

        struct FooterFieldControls
        {
            Microsoft::UI::Xaml::Controls::Border row{ nullptr };
            Microsoft::UI::Xaml::Controls::CheckBox checkbox{ nullptr };
            Microsoft::UI::Xaml::Controls::Button move_up{ nullptr };
            Microsoft::UI::Xaml::Controls::Button move_down{ nullptr };
        };

        void configure_window();
        void refresh_runtime_statuses();
        void refresh_diagnostic_bundle_status();
        void refresh_launch_at_sign_in();
        [[nodiscard]] bool launch_at_sign_in_enabled() const;
        void set_launch_at_sign_in(bool enabled);
        void save_text_preferences();
        void save_appearance_preferences();
        void save_footer_preferences();
        void rebuild_footer_field_rows();
        [[nodiscard]] FooterFieldControls footer_field_controls(glance::app::FooterField field);
        void set_media_volume(
            Microsoft::UI::Xaml::Controls::NumberBox const& control,
            double value,
            std::uint32_t& destination);

        bool initializing_{};
        bool exit_confirmation_open_{};
        bool reset_confirmation_open_{};
        DiagnosticBundleState diagnostic_bundle_state_{};
        std::wstring diagnostic_bundle_path_;
        glance::app::TextPreferences text_preferences_{};
        glance::app::PathCopyPreferences path_copy_preferences_{};
        glance::app::AppearancePreferences appearance_preferences_{};
        glance::app::MediaPreviewPreferences media_preview_preferences_{};
        glance::app::FooterPreferences footer_preferences_{};
        ExitCallback exit_callback_;
        AppearanceChangedCallback appearance_changed_callback_;
        TextPreferencesChangedCallback text_preferences_changed_callback_;
        FooterPreferencesChangedCallback footer_preferences_changed_callback_;
    };
}

namespace winrt::Glance::App::factory_implementation
{
    struct SettingsWindow : SettingsWindowT<SettingsWindow, implementation::SettingsWindow> {};
}

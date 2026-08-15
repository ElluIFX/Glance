#pragma once

#include "SettingsWindow.g.h"
#include "appearance_preferences.h"
#include "component_loader.h"
#include "footer_preferences.h"
#include "media_preview_preferences.h"
#include "path_copy_preferences.h"
#include "text_preferences.h"
#include "update_checker.h"
#include "update_preferences.h"
#include "window_preferences.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

namespace winrt::Glance::App::implementation
{
    struct SettingsWindow : SettingsWindowT<SettingsWindow>
    {
        enum class UpdatePromptResult : std::int32_t
        {
            failed,
            download,
            skip,
            other,
        };

        using ExitCallback = std::function<void()>;
        using AppearanceChangedCallback = std::function<void()>;
        using TextPreferencesChangedCallback = std::function<void()>;
        using FooterPreferencesChangedCallback = std::function<void()>;
        using WindowPreferencesChangedCallback = std::function<void()>;
        using ComponentChangedCallback = std::function<void()>;
        using SourceStatusRequestCallback = std::function<bool(std::string)>;
        using UpdateCheckCallback =
            std::function<glance::contracts::UpdateCheckResult()>;
        using NetworkDownloadCallback = std::function<glance::contracts::NetworkDownloadResult(
            const glance::contracts::NetworkDownloadRequest&,
            const std::atomic_bool&,
            const std::function<void(std::uint64_t, std::uint64_t)>&)>;
        using UpdatePreferencesChangedCallback = std::function<void()>;

        SettingsWindow();
        void InitializeSession(
            ExitCallback exit_callback,
            AppearanceChangedCallback appearance_changed_callback,
            TextPreferencesChangedCallback text_preferences_changed_callback,
            FooterPreferencesChangedCallback footer_preferences_changed_callback,
            WindowPreferencesChangedCallback window_preferences_changed_callback,
            ComponentChangedCallback component_changed_callback,
            SourceStatusRequestCallback source_status_request_callback,
            UpdateCheckCallback update_check_callback,
            NetworkDownloadCallback network_download_callback,
            UpdatePreferencesChangedCallback update_preferences_changed_callback);
        void ApplyAppearancePreferences();
        void ApplyLocalizedResources();
        void ShowAndActivate();
        void ShowComponentAction(
            std::wstring_view component_id,
            std::wstring_view action_id);
        void ShowUpdateDownload(glance::app::UpdateInstallerAsset asset);
        static winrt::Windows::Foundation::IAsyncOperation<std::int32_t>
            ShowUpdateResultDialog(
            Microsoft::UI::Xaml::XamlRoot const& xaml_root,
            glance::app::UpdateCheckResult result,
            bool automatic_prompt);
        void HandleSourceStatuses(std::string_view payload);
        winrt::fire_and_forget ConfirmExit();

        void NumberBox_Loaded(
            IInspectable const& sender,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

        void LaunchAtSignInToggle_Toggled(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void AutomaticUpdateCheckToggle_Toggled(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void UpdateCheckFrequencyComboBox_SelectionChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void DiagnosticsToggle_Toggled(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void WindowPreferenceToggle_Toggled(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void WindowNumberBox_ValueChanged(
            IInspectable const& sender,
            Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const&);
        void AutoFitIgnoredExtensionsTextBox_TextChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
        void DefaultAudioVolumeNumberBox_ValueChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const&);
        void DefaultVideoVolumeNumberBox_ValueChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const&);
        void MediaPreferenceToggle_Toggled(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget ExportDiagnosticBundleButton_Click(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget ResetAllSettingsButton_Click(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget CheckForUpdatesButton_Click(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void CancelUpdateButton_Click(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OpenComponentsFolderButton_Click(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OpenSourcesFolderButton_Click(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ResetWindowSizesButton_Click(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ResetWindowPositionsButton_Click(
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
        winrt::fire_and_forget ExitButton_Tapped(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const&);
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
        void refresh_component_statuses();
        void request_source_statuses();
        void refresh_diagnostic_bundle_status();
        void refresh_launch_at_sign_in();
        [[nodiscard]] bool launch_at_sign_in_enabled() const;
        void set_launch_at_sign_in(bool enabled);
        void save_text_preferences();
        void save_appearance_preferences();
        void save_footer_preferences();
        void update_auto_fit_controls_enabled() noexcept;
        void rebuild_footer_field_rows();
        void rebuild_component_settings();
        [[nodiscard]] FooterFieldControls footer_field_controls(glance::app::FooterField field);
        winrt::fire_and_forget download_and_install_update(glance::app::UpdateInstallerAsset asset);
        winrt::fire_and_forget run_component_action(
            glance::app::ComponentManagementAction action);
        void show_update_download_card(std::wstring_view version);
        void show_download_card(std::wstring_view title, std::wstring_view message);
        void show_preparing_card(std::wstring_view title, std::wstring_view message);
        void set_update_progress(std::uint64_t downloaded, std::uint64_t total);
        void advance_update_progress();
        void show_update_installing_card();
        void cancel_update_download();
        void hide_update_card();
        void set_media_volume(
            Microsoft::UI::Xaml::Controls::NumberBox const& control,
            double value,
            std::uint32_t& destination);

        bool initializing_{};
        bool exit_confirmation_open_{};
        bool reset_confirmation_open_{};
        bool update_check_in_progress_{};
        bool update_download_in_progress_{};
        bool update_installing_{};
        bool update_animations_enabled_{ true };
        std::shared_ptr<std::atomic_bool> update_download_cancellation_;
        Microsoft::UI::Xaml::DispatcherTimer update_progress_timer_{ nullptr };
        double update_displayed_progress_{};
        double update_start_progress_{};
        double update_target_progress_{};
        ULONGLONG update_animation_started_ms_{};
        std::uint64_t update_total_bytes_{};
        DiagnosticBundleState diagnostic_bundle_state_{};
        std::wstring diagnostic_bundle_path_;
        glance::app::TextPreferences text_preferences_{};
        glance::app::PathCopyPreferences path_copy_preferences_{};
        glance::app::AppearancePreferences appearance_preferences_{};
        glance::app::UpdatePreferences update_preferences_{};
        glance::app::MediaPreviewPreferences media_preview_preferences_{};
        glance::app::WindowPreferences window_preferences_{};
        glance::app::FooterPreferences footer_preferences_{};
        ExitCallback exit_callback_;
        AppearanceChangedCallback appearance_changed_callback_;
        TextPreferencesChangedCallback text_preferences_changed_callback_;
        FooterPreferencesChangedCallback footer_preferences_changed_callback_;
        WindowPreferencesChangedCallback window_preferences_changed_callback_;
        ComponentChangedCallback component_changed_callback_;
        SourceStatusRequestCallback source_status_request_callback_;
        UpdateCheckCallback update_check_callback_;
        NetworkDownloadCallback network_download_callback_;
        UpdatePreferencesChangedCallback update_preferences_changed_callback_;
    };
}

namespace winrt::Glance::App::factory_implementation
{
    struct SettingsWindow : SettingsWindowT<SettingsWindow, implementation::SettingsWindow> {};
}

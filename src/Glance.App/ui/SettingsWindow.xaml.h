#pragma once

#include "SettingsWindow.g.h"
#include "settings_registry.h"
#include "localization.h"
#include "appearance_preferences.h"
#include "component_loader.h"
#include "footer_preferences.h"
#include "media_preview_preferences.h"
#include "path_copy_preferences.h"
#include "text_preferences.h"
#include "update_checker.h"
#include "update_preferences.h"
#include "window_preferences.h"
#include "window_acrylic_backdrop.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <map>

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
        using ComponentSettingChangedCallback =
            std::function<void(std::wstring)>;
        using SourceStatusRequestCallback = std::function<bool(std::string)>;
        using UpdateCheckCallback =
            std::function<glance::contracts::UpdateCheckResult()>;
        using NetworkDownloadCallback = std::function<glance::contracts::NetworkDownloadResult(
            const glance::contracts::NetworkDownloadRequest&,
            const std::atomic_bool&,
            const std::function<void(std::uint64_t, std::uint64_t)>&)>;
        using UpdatePreferencesChangedCallback = std::function<void()>;

        SettingsWindow();
        winrt::fire_and_forget RepairCoreAccessButton_Click(
            winrt::Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void InitializeSession(
            ExitCallback exit_callback,
            AppearanceChangedCallback appearance_changed_callback,
            TextPreferencesChangedCallback text_preferences_changed_callback,
            FooterPreferencesChangedCallback footer_preferences_changed_callback,
            WindowPreferencesChangedCallback window_preferences_changed_callback,
            ComponentSettingChangedCallback component_setting_changed_callback,
            SourceStatusRequestCallback source_status_request_callback,
            UpdateCheckCallback update_check_callback,
            NetworkDownloadCallback network_download_callback,
            UpdatePreferencesChangedCallback update_preferences_changed_callback);
        void ApplyAppearancePreferences();
        void ReloadPreferences(std::wstring_view page = {});
        void ApplyLocalizedResources();
        void ShowAndActivate();
        void refresh_runtime_statuses();
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
        void CopyCliPathButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
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
        void OpenDependenciesFolderButton_Click(
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
        void MarkdownSizeNumberBox_ValueChanged(IInspectable const&, Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const&);
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
        void FooterField_ItemClick(
            IInspectable const&,
            Microsoft::UI::Xaml::Controls::ItemClickEventArgs const&);
        void FooterFields_DragItemsCompleted(
            Microsoft::UI::Xaml::Controls::ListViewBase const&,
            Microsoft::UI::Xaml::Controls::DragItemsCompletedEventArgs const&);
        void FooterFields_DragItemsStarting(
            IInspectable const&,
            Microsoft::UI::Xaml::Controls::DragItemsStartingEventArgs const&);
        void FooterFields_DragOver(IInspectable const&, Microsoft::UI::Xaml::DragEventArgs const&);
        void FooterFields_Drop(IInspectable const&, Microsoft::UI::Xaml::DragEventArgs const&);
        void FooterFields_SizeChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        void FooterFields_ContainerContentChanging(
            Microsoft::UI::Xaml::Controls::ListViewBase const&,
            Microsoft::UI::Xaml::Controls::ContainerContentChangingEventArgs const&);
        void AppearanceComboBox_SelectionChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void AcrylicMaterialToggle_Toggled(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void AcrylicOpacitySlider_ValueChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&);
        void SettingsNavigation_SelectionChanged(
            Microsoft::UI::Xaml::Controls::NavigationView const&,
            Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const&);
        winrt::fire_and_forget ExitButton_Tapped(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const&);
        void CloseSettingsButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        glance::app::SettingsRegistry settings_registry_{ [](std::wstring_view owner, std::wstring_view key) { return hstring(owner.empty() ? glance::app::localize(key) : glance::app::localize_component(owner, key)); } };
        void register_general_settings();
        void register_window_settings();
        void register_footer_settings();
        void register_text_settings();
        void register_media_settings();
        void register_components_settings();
        void register_maintenance_settings();
        Microsoft::UI::Xaml::Controls::ComboBox LanguageComboBox() { return settings_registry_.control(L"LanguageComboBox").as<Microsoft::UI::Xaml::Controls::ComboBox>(); }
        Microsoft::UI::Xaml::Controls::ComboBoxItem EnglishLanguageItem() { return settings_registry_.control(L"EnglishLanguageItem").as<Microsoft::UI::Xaml::Controls::ComboBoxItem>(); }
        Microsoft::UI::Xaml::Controls::ComboBoxItem ChineseLanguageItem() { return settings_registry_.control(L"ChineseLanguageItem").as<Microsoft::UI::Xaml::Controls::ComboBoxItem>(); }
        Microsoft::UI::Xaml::Controls::ComboBox ThemeComboBox() { return settings_registry_.control(L"ThemeComboBox").as<Microsoft::UI::Xaml::Controls::ComboBox>(); }
        Microsoft::UI::Xaml::Controls::ComboBox AccentComboBox() { return settings_registry_.control(L"AccentComboBox").as<Microsoft::UI::Xaml::Controls::ComboBox>(); }
        Microsoft::UI::Xaml::Controls::TextBlock AccentSystemText() { return settings_registry_.control(L"AccentSystemText").as<Microsoft::UI::Xaml::Controls::TextBlock>(); }
        Microsoft::UI::Xaml::Controls::TextBlock AccentBlueText() { return settings_registry_.control(L"AccentBlueText").as<Microsoft::UI::Xaml::Controls::TextBlock>(); }
        Microsoft::UI::Xaml::Controls::TextBlock AccentTealText() { return settings_registry_.control(L"AccentTealText").as<Microsoft::UI::Xaml::Controls::TextBlock>(); }
        Microsoft::UI::Xaml::Controls::TextBlock AccentGreenText() { return settings_registry_.control(L"AccentGreenText").as<Microsoft::UI::Xaml::Controls::TextBlock>(); }
        Microsoft::UI::Xaml::Controls::TextBlock AccentOrangeText() { return settings_registry_.control(L"AccentOrangeText").as<Microsoft::UI::Xaml::Controls::TextBlock>(); }
        Microsoft::UI::Xaml::Controls::TextBlock AccentRedText() { return settings_registry_.control(L"AccentRedText").as<Microsoft::UI::Xaml::Controls::TextBlock>(); }
        Microsoft::UI::Xaml::Controls::TextBlock AccentPinkText() { return settings_registry_.control(L"AccentPinkText").as<Microsoft::UI::Xaml::Controls::TextBlock>(); }
        Microsoft::UI::Xaml::Controls::TextBlock AccentPurpleText() { return settings_registry_.control(L"AccentPurpleText").as<Microsoft::UI::Xaml::Controls::TextBlock>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch AcrylicMaterialToggle() { return settings_registry_.control(L"AcrylicMaterialToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::Slider AcrylicOpacitySlider() { return settings_registry_.control(L"AcrylicOpacitySlider").as<Microsoft::UI::Xaml::Controls::Slider>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch LaunchAtSignInToggle() { return settings_registry_.control(L"LaunchAtSignInToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch AutomaticUpdateCheckToggle() { return settings_registry_.control(L"AutomaticUpdateCheckToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::ComboBox UpdateCheckFrequencyComboBox() { return settings_registry_.control(L"UpdateCheckFrequencyComboBox").as<Microsoft::UI::Xaml::Controls::ComboBox>(); }
        Microsoft::UI::Xaml::Controls::ComboBoxItem UpdateFrequencyHourlyItem() { return settings_registry_.control(L"UpdateFrequencyHourlyItem").as<Microsoft::UI::Xaml::Controls::ComboBoxItem>(); }
        Microsoft::UI::Xaml::Controls::ComboBoxItem UpdateFrequencyDailyItem() { return settings_registry_.control(L"UpdateFrequencyDailyItem").as<Microsoft::UI::Xaml::Controls::ComboBoxItem>(); }
        Microsoft::UI::Xaml::Controls::ComboBoxItem UpdateFrequencyWeeklyItem() { return settings_registry_.control(L"UpdateFrequencyWeeklyItem").as<Microsoft::UI::Xaml::Controls::ComboBoxItem>(); }
        Microsoft::UI::Xaml::Controls::ComboBoxItem UpdateFrequencyMonthlyItem() { return settings_registry_.control(L"UpdateFrequencyMonthlyItem").as<Microsoft::UI::Xaml::Controls::ComboBoxItem>(); }
        Microsoft::UI::Xaml::Controls::NumberBox DefaultWindowWidthNumberBox() { return settings_registry_.control(L"DefaultWindowWidthNumberBox").as<Microsoft::UI::Xaml::Controls::NumberBox>(); }
        Microsoft::UI::Xaml::Controls::NumberBox DefaultWindowHeightNumberBox() { return settings_registry_.control(L"DefaultWindowHeightNumberBox").as<Microsoft::UI::Xaml::Controls::NumberBox>(); }
        Microsoft::UI::Xaml::Controls::Button ResetWindowSizesButton() { return settings_registry_.control(L"ResetWindowSizesButton").as<Microsoft::UI::Xaml::Controls::Button>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch RememberWindowSizeToggle() { return settings_registry_.control(L"RememberWindowSizeToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::Button ResetWindowPositionsButton() { return settings_registry_.control(L"ResetWindowPositionsButton").as<Microsoft::UI::Xaml::Controls::Button>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch RememberWindowPositionToggle() { return settings_registry_.control(L"RememberWindowPositionToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch DoubleClickFullscreenToggle() { return settings_registry_.control(L"DoubleClickFullscreenToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch RightClickCloseToggle() { return settings_registry_.control(L"RightClickCloseToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch AutoFitWindowSizeToggle() { return settings_registry_.control(L"AutoFitWindowSizeToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch PauseAutoFitWhenTopmostToggle() { return settings_registry_.control(L"PauseAutoFitWhenTopmostToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch PauseAutoFitInGalleryToggle() { return settings_registry_.control(L"PauseAutoFitInGalleryToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch ShowAfterAutoFitToggle() { return settings_registry_.control(L"ShowAfterAutoFitToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch DynamicAutoFitToggle() { return settings_registry_.control(L"DynamicAutoFitToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::NumberBox AdaptiveMinimumPercentNumberBox() { return settings_registry_.control(L"AdaptiveMinimumPercentNumberBox").as<Microsoft::UI::Xaml::Controls::NumberBox>(); }
        Microsoft::UI::Xaml::Controls::NumberBox AdaptiveMaximumPercentNumberBox() { return settings_registry_.control(L"AdaptiveMaximumPercentNumberBox").as<Microsoft::UI::Xaml::Controls::NumberBox>(); }
        Microsoft::UI::Xaml::Controls::TextBox AutoFitIgnoredExtensionsTextBox() { return settings_registry_.control(L"AutoFitIgnoredExtensionsTextBox").as<Microsoft::UI::Xaml::Controls::TextBox>(); }
        void show_window_reset_result(bool sizes, bool succeeded);
        Microsoft::UI::Xaml::DispatcherTimer size_reset_timer_{ nullptr };
        Microsoft::UI::Xaml::DispatcherTimer position_reset_timer_{ nullptr };
        std::wstring size_reset_text_key_;
        std::wstring position_reset_text_key_;
        Microsoft::UI::Xaml::Controls::GridView FooterEnabledFields() { return settings_registry_.control(L"FooterEnabledFields").as<Microsoft::UI::Xaml::Controls::GridView>(); }
        Microsoft::UI::Xaml::Controls::GridView FooterDisabledFields() { return settings_registry_.control(L"FooterDisabledFields").as<Microsoft::UI::Xaml::Controls::GridView>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch QuoteCopiedPathToggle() { return settings_registry_.control(L"QuoteCopiedPathToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch UnixPathSeparatorsToggle() { return settings_registry_.control(L"UnixPathSeparatorsToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::ComboBox FontFamilyComboBox() { return settings_registry_.control(L"FontFamilyComboBox").as<Microsoft::UI::Xaml::Controls::ComboBox>(); }
        Microsoft::UI::Xaml::Controls::NumberBox FontSizeNumberBox() { return settings_registry_.control(L"FontSizeNumberBox").as<Microsoft::UI::Xaml::Controls::NumberBox>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch SyntaxHighlightingToggle() { return settings_registry_.control(L"SyntaxHighlightingToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::ComboBox SyntaxThemeComboBox() { return settings_registry_.control(L"SyntaxThemeComboBox").as<Microsoft::UI::Xaml::Controls::ComboBox>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch LineNumbersToggle() { return settings_registry_.control(L"LineNumbersToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch WordWrapToggle() { return settings_registry_.control(L"WordWrapToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch MarkdownDefaultPreviewToggle() { return settings_registry_.control(L"MarkdownDefaultPreviewToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::ComboBox MarkdownFontComboBox() { return settings_registry_.control(L"MarkdownFontComboBox").as<Microsoft::UI::Xaml::Controls::ComboBox>(); }
        Microsoft::UI::Xaml::Controls::NumberBox MarkdownSizeNumberBox() { return settings_registry_.control(L"MarkdownSizeNumberBox").as<Microsoft::UI::Xaml::Controls::NumberBox>(); }
        Microsoft::UI::Xaml::Controls::ComboBox MarkdownStyleComboBox() { return settings_registry_.control(L"MarkdownStyleComboBox").as<Microsoft::UI::Xaml::Controls::ComboBox>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch JsonDefaultTreeToggle() { return settings_registry_.control(L"JsonDefaultTreeToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::NumberBox DefaultAudioVolumeNumberBox() { return settings_registry_.control(L"DefaultAudioVolumeNumberBox").as<Microsoft::UI::Xaml::Controls::NumberBox>(); }
        Microsoft::UI::Xaml::Controls::NumberBox DefaultVideoVolumeNumberBox() { return settings_registry_.control(L"DefaultVideoVolumeNumberBox").as<Microsoft::UI::Xaml::Controls::NumberBox>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch AutoplayAudioToggle() { return settings_registry_.control(L"AutoplayAudioToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch AutoplayVideoToggle() { return settings_registry_.control(L"AutoplayVideoToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch LoopPlaybackToggle() { return settings_registry_.control(L"LoopPlaybackToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch ReverseSeekWheelToggle() { return settings_registry_.control(L"ReverseSeekWheelToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch PreferFFmpegToggle() { return settings_registry_.control(L"PreferFFmpegToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch ImageZoomMapToggle() { return settings_registry_.control(L"ImageZoomMapToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch MiddleClickGalleryModeToggle() { return settings_registry_.control(L"MiddleClickGalleryModeToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch LoopGalleryScrollingToggle() { return settings_registry_.control(L"LoopGalleryScrollingToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch GallerySameExtensionOnlyToggle() { return settings_registry_.control(L"GallerySameExtensionOnlyToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::Button OpenDependenciesFolderButton() { return settings_registry_.control(L"OpenDependenciesFolderButton").as<Microsoft::UI::Xaml::Controls::Button>(); }
        Microsoft::UI::Xaml::Controls::TextBlock DependencyStatusGroupTitle() { return settings_registry_.control(L"DependencyStatusGroupTitle").as<Microsoft::UI::Xaml::Controls::TextBlock>(); }
        Microsoft::UI::Xaml::Controls::StackPanel DependencyStatusList() { return settings_registry_.control(L"DependencyStatusList").as<Microsoft::UI::Xaml::Controls::StackPanel>(); }
        Microsoft::UI::Xaml::Controls::Button OpenComponentsFolderButton() { return settings_registry_.control(L"OpenComponentsFolderButton").as<Microsoft::UI::Xaml::Controls::Button>(); }
        Microsoft::UI::Xaml::Controls::TextBlock ComponentStatusGroupTitle() { return settings_registry_.control(L"ComponentStatusGroupTitle").as<Microsoft::UI::Xaml::Controls::TextBlock>(); }
        Microsoft::UI::Xaml::Controls::Grid ComponentEmptyState() { return settings_registry_.control(L"ComponentEmptyState").as<Microsoft::UI::Xaml::Controls::Grid>(); }
        Microsoft::UI::Xaml::Controls::TextBlock ComponentEmptyTitle() { return settings_registry_.control(L"ComponentEmptyTitle").as<Microsoft::UI::Xaml::Controls::TextBlock>(); }
        Microsoft::UI::Xaml::Controls::TextBlock ComponentEmptyDescription() { return settings_registry_.control(L"ComponentEmptyDescription").as<Microsoft::UI::Xaml::Controls::TextBlock>(); }
        Microsoft::UI::Xaml::Controls::Grid ComponentStatusScroller() { return settings_registry_.control(L"ComponentStatusScroller").as<Microsoft::UI::Xaml::Controls::Grid>(); }
        Microsoft::UI::Xaml::Controls::StackPanel ComponentStatusList() { return settings_registry_.control(L"ComponentStatusList").as<Microsoft::UI::Xaml::Controls::StackPanel>(); }
        Microsoft::UI::Xaml::Controls::Button OpenSourcesFolderButton() { return settings_registry_.control(L"OpenSourcesFolderButton").as<Microsoft::UI::Xaml::Controls::Button>(); }
        Microsoft::UI::Xaml::Controls::TextBlock SourceStatusGroupTitle() { return settings_registry_.control(L"SourceStatusGroupTitle").as<Microsoft::UI::Xaml::Controls::TextBlock>(); }
        Microsoft::UI::Xaml::Controls::Grid SourceEmptyState() { return settings_registry_.control(L"SourceEmptyState").as<Microsoft::UI::Xaml::Controls::Grid>(); }
        Microsoft::UI::Xaml::Controls::TextBlock SourceEmptyTitle() { return settings_registry_.control(L"SourceEmptyTitle").as<Microsoft::UI::Xaml::Controls::TextBlock>(); }
        Microsoft::UI::Xaml::Controls::TextBlock SourceEmptyDescription() { return settings_registry_.control(L"SourceEmptyDescription").as<Microsoft::UI::Xaml::Controls::TextBlock>(); }
        Microsoft::UI::Xaml::Controls::Grid SourceStatusScroller() { return settings_registry_.control(L"SourceStatusScroller").as<Microsoft::UI::Xaml::Controls::Grid>(); }
        Microsoft::UI::Xaml::Controls::StackPanel SourceStatusList() { return settings_registry_.control(L"SourceStatusList").as<Microsoft::UI::Xaml::Controls::StackPanel>(); }
        Microsoft::UI::Xaml::Controls::TextBlock CoreStatusText() { return settings_registry_.item_description(L"InputCoreLabel").as<Microsoft::UI::Xaml::Controls::TextBlock>(); }
        Microsoft::UI::Xaml::Controls::FontIcon CoreStatusIcon() { return settings_registry_.control(L"CoreStatusIcon").as<Microsoft::UI::Xaml::Controls::FontIcon>(); }
        Microsoft::UI::Xaml::Controls::TextBlock AdministratorAccessStatusText() { return settings_registry_.item_description(L"AdministratorAccessLabel").as<Microsoft::UI::Xaml::Controls::TextBlock>(); }
        Microsoft::UI::Xaml::Controls::Button RepairCoreAccessButton() { return settings_registry_.control(L"RepairCoreAccessButton").as<Microsoft::UI::Xaml::Controls::Button>(); }
        Microsoft::UI::Xaml::Controls::FontIcon AdministratorAccessStatusIcon() { return settings_registry_.control(L"AdministratorAccessStatusIcon").as<Microsoft::UI::Xaml::Controls::FontIcon>(); }
        Microsoft::UI::Xaml::Controls::TextBlock WebViewAvailabilityStatusText() { return settings_registry_.item_description(L"WebViewAvailabilityLabel").as<Microsoft::UI::Xaml::Controls::TextBlock>(); }
        Microsoft::UI::Xaml::Controls::HyperlinkButton WebViewDownloadLink() { return settings_registry_.control(L"WebViewDownloadLink").as<Microsoft::UI::Xaml::Controls::HyperlinkButton>(); }
        Microsoft::UI::Xaml::Controls::FontIcon WebViewAvailabilityStatusIcon() { return settings_registry_.control(L"WebViewAvailabilityStatusIcon").as<Microsoft::UI::Xaml::Controls::FontIcon>(); }
        Microsoft::UI::Xaml::Controls::ToggleSwitch DiagnosticsToggle() { return settings_registry_.control(L"DiagnosticsToggle").as<Microsoft::UI::Xaml::Controls::ToggleSwitch>(); }
        Microsoft::UI::Xaml::Controls::TextBlock DiagnosticBundleStatusText() { return settings_registry_.item_description(L"DiagnosticBundleLabel").as<Microsoft::UI::Xaml::Controls::TextBlock>(); }
        Microsoft::UI::Xaml::Controls::Button ExportDiagnosticBundleButton() { return settings_registry_.control(L"ExportDiagnosticBundleButton").as<Microsoft::UI::Xaml::Controls::Button>(); }
        Microsoft::UI::Xaml::Controls::Button ResetAllSettingsButton() { return settings_registry_.control(L"ResetAllSettingsButton").as<Microsoft::UI::Xaml::Controls::Button>(); }
        Microsoft::UI::Xaml::Controls::Button CopyCliPathButton() { return settings_registry_.control(L"CopyCliPathButton").as<Microsoft::UI::Xaml::Controls::Button>(); }

        enum class DiagnosticBundleState
        {
            idle,
            packaging,
            succeeded,
            failed,
        };

        void configure_window();
        void refresh_component_statuses();
        void refresh_dependency_statuses();
        winrt::fire_and_forget uninstall_dependency(std::wstring id);
        void request_source_statuses();
        void refresh_diagnostic_bundle_status();
        void refresh_launch_at_sign_in();
        [[nodiscard]] bool launch_at_sign_in_enabled() const;
        void set_launch_at_sign_in(bool enabled);
        void save_text_preferences();
        void save_appearance_preferences();
        void apply_background_surface(bool acrylic_enabled);
        void update_acrylic_dependency(bool animate);
        void update_update_frequency_dependency(bool animate);
        void update_auto_fit_dependency(bool animate);
        void refresh_toggle_descriptions();
        void save_footer_preferences();
        void rebuild_footer_field_cards();
        void update_footer_field_heights();
        void update_footer_field_order();
        void toggle_footer_field(Microsoft::UI::Xaml::Controls::Border const& card);
        Microsoft::UI::Xaml::Controls::Border dragged_footer_card_{ nullptr };
        void register_component_settings();
        winrt::fire_and_forget download_and_install_update(glance::app::UpdateInstallerAsset asset);
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

        bool initializing_{ true };
        bool exit_confirmation_open_{};
        bool reset_confirmation_open_{};
        bool update_check_in_progress_{};
        bool update_download_in_progress_{};
        bool update_installing_{};
        bool update_animations_enabled_{ true };
        winrt::event_token first_frame_rendered_token_{};
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
        std::unique_ptr<glance::app::WindowAcrylicBackdrop> acrylic_backdrop_;
        glance::app::UpdatePreferences update_preferences_{};
        glance::app::MediaPreviewPreferences media_preview_preferences_{};
        glance::app::WindowPreferences window_preferences_{};
        glance::app::FooterPreferences footer_preferences_{};
        ExitCallback exit_callback_;
        AppearanceChangedCallback appearance_changed_callback_;
        TextPreferencesChangedCallback text_preferences_changed_callback_;
        FooterPreferencesChangedCallback footer_preferences_changed_callback_;
        WindowPreferencesChangedCallback window_preferences_changed_callback_;
        ComponentSettingChangedCallback component_setting_changed_callback_;
        SourceStatusRequestCallback source_status_request_callback_;
        UpdateCheckCallback update_check_callback_;
        NetworkDownloadCallback network_download_callback_;
        std::map<std::wstring, std::wstring> dependency_errors_;
        struct AddonRow
        {
            glance::app::SettingsRowView view;
            Microsoft::UI::Xaml::Controls::FontIcon icon{ nullptr };
            Microsoft::UI::Xaml::Controls::TextBlock status{ nullptr };
            Microsoft::UI::Xaml::Controls::Button action{ nullptr };
            Microsoft::UI::Xaml::Controls::ProgressBar progress{ nullptr };
            std::weak_ptr<void> observed_transfer;
        };
        std::map<std::wstring, AddonRow> dependency_rows_, component_rows_, source_rows_;
        UpdatePreferencesChangedCallback update_preferences_changed_callback_;
    };
}

namespace winrt::Glance::App::factory_implementation
{
    struct SettingsWindow : SettingsWindowT<SettingsWindow, implementation::SettingsWindow> {};
}

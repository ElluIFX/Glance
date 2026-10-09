#include "pch.h"
#include "SettingsWindow.xaml.h"
#include "App.xaml.h"
#include "appearance_preferences.h"
#include "component_loader.h"
#include "dependencies/dependency_service.h"
#include "footer_preferences.h"
#include "localization.h"
#include "path_copy_preferences.h"
#include "resource.h"
#include "startup_registration.h"
#include "text_font_fallback.h"
#include "text_preferences.h"
#include "update_checker.h"
#include "webview_availability.h"
#include "window_size_store.h"
#include "glance/contracts/diagnostics.h"
#include "glance/contracts/storage.h"
#include "../../version.h"

#include <microsoft.ui.xaml.window.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Composition.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <cmath>
#include <optional>
#include <ranges>
#include <string_view>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
namespace Controls = Microsoft::UI::Xaml::Controls;
namespace Media = Microsoft::UI::Xaml::Media;
namespace Shapes = Microsoft::UI::Xaml::Shapes;

#include <winrt/Microsoft.UI.Xaml.Markup.h>

namespace winrt::Glance::App::implementation
{

    void SettingsWindow::ApplyLocalizedResources()
    {
        const bool initial_state = initializing_;
        initializing_ = true;
        const auto set_text = [](const auto &control, wchar_t const *key) { control.Text(glance::app::localize(key)); };
        const auto set_content = [](const auto &control, wchar_t const *key) {
            control.Content(box_value(glance::app::localize(key)));
        };

        settings_registry_.localize();
        Title(glance::app::localize(L"SettingsTitle"));
        set_text(SettingsTitleText(), L"SettingsTitleText.Text");
        Controls::ToolTipService::SetToolTip(
            CloseSettingsButton(), box_value(glance::app::localize(L"CloseSettingsButton.ToolTipService.ToolTip")));
        set_content(AboutNavigationItem(), L"AboutNavigationItem.Content");
        set_text(ExitButtonText(), L"ExitButtonText.Text");
        if (settings_registry_.page_created(L"footer"))
            rebuild_footer_field_cards();
        if (settings_registry_.page_created(L"window"))
            AutoFitIgnoredExtensionsTextBox().PlaceholderText(
                glance::app::localize(L"AutoFitIgnoredExtensionsTextBox.PlaceholderText"));
        if (settings_registry_.page_created(L"maintenance"))
            RepairCoreAccessButton().Content(box_value(glance::app::localize(L"RepairCoreAccessButton.Content")));
        refresh_diagnostic_bundle_status();
        set_text(AboutPageTitle(), L"AboutPageTitle.Text");
        set_text(AboutPageDescription(), L"AboutPageDescription.Text");
        set_text(AboutAuthorLabel(), L"AboutAuthorLabel.Text");
        set_text(AboutLicenseText(), L"AboutLicenseText.Text");
        set_content(AboutProjectLink(), L"AboutProjectLink.Content");
        set_content(CheckForUpdatesButton(), L"CheckForUpdatesButton.Content");
        set_content(CancelUpdateButton(), L"Cancel");
        AboutVersionText().Text(glance::app::localize_format(L"VersionFormat", {GLANCE_VERSION_WSTRING}) + L" " +
                                glance::app::localize(glance::contracts::storage::portable
                                                          ? L"DistributionPortable" : L"DistributionInstalled"));
        refresh_runtime_statuses();
        refresh_component_statuses();
        request_source_statuses();
        settings_registry_.refresh();
        refresh_toggle_descriptions();
        initializing_ = initial_state;
    }
    void SettingsWindow::ReloadPreferences(std::wstring_view page)
    {
        const bool was_initializing = initializing_;
        initializing_ = true;
        appearance_preferences_ = glance::app::load_appearance_preferences();
        update_preferences_ = glance::app::load_update_preferences();
        window_preferences_ = glance::app::load_window_preferences();
        media_preview_preferences_ = glance::app::load_media_preview_preferences();
        text_preferences_ = glance::app::load_text_preferences();
        path_copy_preferences_ = glance::app::load_path_copy_preferences();
        footer_preferences_ = glance::app::load_footer_preferences();
        const auto refresh_page = [&](std::wstring_view id) {
            return (page.empty() || page == id) && settings_registry_.page_created(id);
        };
        if (refresh_page(L"general"))
        {
            LanguageComboBox().SelectedIndex(appearance_preferences_.language == L"zh-CN" ? 1 : 0);
            ThemeComboBox().SelectedIndex(static_cast<int>(appearance_preferences_.theme));
            AccentComboBox().SelectedIndex(static_cast<int>(appearance_preferences_.accent));
            const bool acrylic_supported = glance::app::acrylic_material_supported();
            AcrylicOpacitySlider().Value(appearance_preferences_.acrylic_opacity_percent);
            if (acrylic_supported)
            {
                AcrylicMaterialToggle().IsOn(appearance_preferences_.acrylic_enabled);
            }
            LaunchAtSignInToggle().IsOn(launch_at_sign_in_enabled());
            AutomaticUpdateCheckToggle().IsOn(update_preferences_.automatic_check_enabled);
            UpdateCheckFrequencyComboBox().SelectedIndex(static_cast<int>(update_preferences_.frequency));
        }
        if (refresh_page(L"window"))
        {
            AutoFitIgnoredExtensionsTextBox().PlaceholderText(
                glance::app::localize(L"AutoFitIgnoredExtensionsTextBox.PlaceholderText"));
            DefaultWindowWidthNumberBox().Value(window_preferences_.default_width);
            DefaultWindowHeightNumberBox().Value(window_preferences_.default_height);
            RememberWindowSizeToggle().IsOn(window_preferences_.remember_size);
            AutoFitWindowSizeToggle().IsOn(window_preferences_.auto_fit_media);
            PauseAutoFitWhenTopmostToggle().IsOn(window_preferences_.pause_auto_fit_when_topmost);
            PauseAutoFitInGalleryToggle().IsOn(window_preferences_.pause_auto_fit_in_gallery);
            ShowAfterAutoFitToggle().IsOn(window_preferences_.show_after_auto_fit);
            DynamicAutoFitToggle().IsOn(window_preferences_.dynamic_auto_fit);
            AdaptiveMinimumPercentNumberBox().Value(window_preferences_.adaptive_minimum_percent);
            AdaptiveMaximumPercentNumberBox().Value(window_preferences_.adaptive_maximum_percent);
            AutoFitIgnoredExtensionsTextBox().Text(window_preferences_.auto_fit_ignored_extensions);
            RememberWindowPositionToggle().IsOn(window_preferences_.remember_position);
            DoubleClickFullscreenToggle().IsOn(window_preferences_.double_click_fullscreen);
            RightClickCloseToggle().IsOn(window_preferences_.right_click_close);
        }
        if (refresh_page(L"media"))
        {
            DefaultAudioVolumeNumberBox().Value(media_preview_preferences_.audio_volume_percent);
            DefaultVideoVolumeNumberBox().Value(media_preview_preferences_.video_volume_percent);
            AutoplayAudioToggle().IsOn(media_preview_preferences_.autoplay_audio);
            AutoplayVideoToggle().IsOn(media_preview_preferences_.autoplay_video);
            LoopPlaybackToggle().IsOn(media_preview_preferences_.loop_playback);
            PreferFFmpegToggle().IsOn(media_preview_preferences_.prefer_ffmpeg);
            ReverseSeekWheelToggle().IsOn(media_preview_preferences_.reverse_seek_wheel);
            MiddleClickGalleryModeToggle().IsOn(media_preview_preferences_.middle_click_gallery_mode);
            LoopGalleryScrollingToggle().IsOn(media_preview_preferences_.loop_gallery_scrolling);
            GallerySameExtensionOnlyToggle().IsOn(media_preview_preferences_.gallery_same_extension_only);
            ImageZoomMapToggle().IsOn(media_preview_preferences_.show_image_zoom_map);
        }
        if (refresh_page(L"text"))
        {
            FontFamilyComboBox().Items().Clear();
            MarkdownFontComboBox().Items().Clear();
            auto font_families = glance::app::system_font_families();
            if (font_families.empty())
            {
                for (const auto font_family : glance::app::preferred_text_font_families)
                {
                    font_families.emplace_back(font_family);
                }
            }
            int selected_font = -1;
            for (std::size_t index = 0; index < font_families.size(); ++index)
            {
                FontFamilyComboBox().Items().Append(box_value(font_families[index]));
                if (_wcsicmp(font_families[index].c_str(), text_preferences_.font_family.c_str()) == 0)
                {
                    selected_font = static_cast<int>(index);
                }
            }
            if (selected_font < 0)
            {
                selected_font = static_cast<int>(font_families.size());
                FontFamilyComboBox().Items().Append(box_value(text_preferences_.font_family));
            }
            FontFamilyComboBox().SelectedIndex(selected_font);
            int markdown_font = -1;
            for (std::size_t index = 0; index < font_families.size(); ++index)
            {
                MarkdownFontComboBox().Items().Append(box_value(font_families[index]));
                if (_wcsicmp(font_families[index].c_str(), text_preferences_.markdown_font_family.c_str()) == 0)
                    markdown_font = static_cast<int>(index);
            }
            if (markdown_font < 0)
            {
                markdown_font = static_cast<int>(font_families.size());
                MarkdownFontComboBox().Items().Append(box_value(text_preferences_.markdown_font_family));
            }
            MarkdownFontComboBox().SelectedIndex(markdown_font);
            MarkdownSizeNumberBox().Value(text_preferences_.markdown_font_size);
            MarkdownStyleComboBox().SelectedIndex(static_cast<int>(text_preferences_.markdown_style));
            MarkdownDefaultPreviewToggle().IsOn(text_preferences_.markdown_default_preview);
            JsonDefaultTreeToggle().IsOn(text_preferences_.json_default_tree);
            FontSizeNumberBox().Value(text_preferences_.font_size);
            SyntaxHighlightingToggle().IsOn(text_preferences_.syntax_highlighting);
            SyntaxThemeComboBox().SelectedIndex(static_cast<int>(text_preferences_.syntax_theme));
            SyntaxThemeComboBox().IsEnabled(text_preferences_.syntax_highlighting);
            LineNumbersToggle().IsOn(text_preferences_.line_numbers);
            WordWrapToggle().IsOn(text_preferences_.word_wrap);
        }
        if (refresh_page(L"footer"))
        {
            QuoteCopiedPathToggle().IsOn(path_copy_preferences_.quote_path);
            UnixPathSeparatorsToggle().IsOn(path_copy_preferences_.use_unix_separators);
            rebuild_footer_field_cards();
        }
        if (refresh_page(L"maintenance"))
        {
            DiagnosticsToggle().IsOn(glance::contracts::diagnostics_enabled());
        }
        settings_registry_.refresh();
        initializing_ = was_initializing;
    }
    void SettingsWindow::refresh_toggle_descriptions()
    {
        settings_registry_.refresh();
    }
} // namespace winrt::Glance::App::implementation

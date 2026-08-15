#include "pch.h"
#include "SettingsWindow.xaml.h"
#include "appearance_preferences.h"
#include "component_loader.h"
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
#include "../../version.h"
#if __has_include("SettingsWindow.g.cpp")
#include "SettingsWindow.g.cpp"
#endif

#include <microsoft.ui.xaml.window.h>
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

namespace
{
    constexpr wchar_t latest_release_url[] =
        L"https://github.com/ElluIFX/Glance/releases/latest";

    std::wstring safe_release_url(std::wstring_view value)
    {
        constexpr std::wstring_view release_prefix =
            L"https://github.com/ElluIFX/Glance/releases/";
        return value.starts_with(release_prefix)
            ? std::wstring(value)
            : std::wstring(latest_release_url);
    }

    std::wstring format_megabytes(std::uint64_t bytes)
    {
        wchar_t text[32]{};
        swprintf_s(text, L"%.1f", static_cast<double>(bytes) / (1024.0 * 1024.0));
        return text;
    }

    bool client_animations_enabled() noexcept
    {
        BOOL enabled = TRUE;
        return !SystemParametersInfoW(
                   SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) || enabled != FALSE;
    }

    void disable_number_box_clear_button(DependencyObject const& root)
    {
        const int count = Media::VisualTreeHelper::GetChildrenCount(root);
        for (int index = 0; index < count; ++index)
        {
            const auto child = Media::VisualTreeHelper::GetChild(root, index);
            if (const auto control = child.try_as<Controls::Control>())
            {
                control.ApplyTemplate();
            }
            if (const auto button = child.try_as<Controls::Button>();
                button != nullptr && button.Name() == L"DeleteButton")
            {
                button.MinWidth(0);
                button.MaxWidth(0);
                button.Width(0);
                button.Padding(Thickness{});
                button.Margin(Thickness{});
                button.IsHitTestVisible(false);
                button.Opacity(0);
                continue;
            }
            disable_number_box_clear_button(child);
        }
    }

    std::wstring quote_argument(std::wstring_view value)
    {
        std::wstring quoted(1, L'"');
        for (const wchar_t character : value)
        {
            if (character == L'"')
            {
                quoted += L"\\\"";
            }
            else
            {
                quoted += character;
            }
        }
        quoted += L'"';
        return quoted;
    }

    std::wstring bundle_timestamp()
    {
        SYSTEMTIME time{};
        GetLocalTime(&time);
        wchar_t value[32]{};
        swprintf_s(
            value,
            L"%04u%02u%02u-%02u%02u%02u",
            time.wYear,
            time.wMonth,
            time.wDay,
            time.wHour,
            time.wMinute,
            time.wSecond);
        return value;
    }

    std::optional<std::filesystem::path> select_output_directory(HWND owner, std::wstring_view title)
    {
        com_ptr<IFileOpenDialog> dialog;
        check_hresult(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())));
        FILEOPENDIALOGOPTIONS options{};
        check_hresult(dialog->GetOptions(&options));
        check_hresult(dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM));
        const std::wstring dialog_title(title);
        check_hresult(dialog->SetTitle(dialog_title.c_str()));
        const HRESULT result = dialog->Show(owner);
        if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        {
            return std::nullopt;
        }
        check_hresult(result);

        com_ptr<IShellItem> item;
        check_hresult(dialog->GetResult(item.put()));
        PWSTR path{};
        check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH, &path));
        const std::filesystem::path selected(path);
        CoTaskMemFree(path);
        return selected;
    }

    std::pair<bool, std::wstring> create_diagnostic_bundle(const std::filesystem::path& output_directory)
    {
        const std::filesystem::path diagnostics_root(glance::contracts::diagnostics_root_path());
        if (diagnostics_root.empty())
        {
            return { false, {} };
        }

        wchar_t system_directory[MAX_PATH]{};
        const UINT system_directory_length = GetSystemDirectoryW(system_directory, ARRAYSIZE(system_directory));
        if (system_directory_length == 0 || system_directory_length >= ARRAYSIZE(system_directory))
        {
            return { false, {} };
        }
        const std::filesystem::path tar_path = std::filesystem::path(system_directory) / L"tar.exe";
        if (!std::filesystem::exists(tar_path))
        {
            return { false, {} };
        }

        const auto output_path = output_directory /
            (L"Glance-Diagnostics-" + bundle_timestamp() + L"-" + std::to_wstring(GetCurrentProcessId()) + L".zip");
        std::wstring command_line = quote_argument(tar_path.wstring()) +
            L" -a -c -f " + quote_argument(output_path.wstring()) +
            L" -C " + quote_argument(diagnostics_root.wstring()) + L" Logs Dumps";
        STARTUPINFOW startup{ sizeof(startup) };
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(
                tar_path.c_str(),
                command_line.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &startup,
                &process))
        {
            return { false, {} };
        }

        CloseHandle(process.hThread);
        const DWORD wait_result = WaitForSingleObject(process.hProcess, 120000);
        DWORD exit_code = ERROR_GEN_FAILURE;
        if (wait_result == WAIT_OBJECT_0)
        {
            GetExitCodeProcess(process.hProcess, &exit_code);
        }
        else if (wait_result == WAIT_TIMEOUT)
        {
            TerminateProcess(process.hProcess, ERROR_TIMEOUT);
            WaitForSingleObject(process.hProcess, 5000);
        }
        CloseHandle(process.hProcess);

        if (wait_result != WAIT_OBJECT_0 || exit_code != 0 || !std::filesystem::exists(output_path))
        {
            std::error_code error;
            std::filesystem::remove(output_path, error);
            return { false, {} };
        }
        return { true, output_path.wstring() };
    }

    bool named_mutex_exists(const wchar_t* name) noexcept
    {
        const HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, name);
        if (mutex == nullptr)
        {
            return false;
        }
        CloseHandle(mutex);
        return true;
    }

    void set_status_indicator(
        const Controls::FontIcon& icon,
        const Controls::TextBlock& text,
        bool available,
        const wchar_t* available_resource,
        const wchar_t* unavailable_resource)
    {
        const auto status = glance::app::localize(
            available ? available_resource : unavailable_resource);
        text.Text(status);
        icon.Glyph(available ? L"\xE8FB" : L"\xE711");
        icon.Foreground(Media::SolidColorBrush(available
            ? Windows::UI::Color{ 255, 16, 124, 16 }
            : Windows::UI::Color{ 255, 196, 43, 28 }));
        Controls::ToolTipService::SetToolTip(icon, box_value(status));
    }

    std::optional<glance::app::FooterField> footer_field_from_tag(
        Windows::Foundation::IInspectable const& value)
    {
        const auto tag = unbox_value_or<hstring>(value, L"");
        if (tag == L"size") return glance::app::FooterField::size;
        if (tag == L"modified") return glance::app::FooterField::modified_time;
        if (tag == L"created") return glance::app::FooterField::creation_time;
        if (tag == L"taken") return glance::app::FooterField::taken_time;
        if (tag == L"permissions") return glance::app::FooterField::permissions;
        if (tag == L"media") return glance::app::FooterField::media_info;
        return std::nullopt;
    }
}

namespace winrt::Glance::App::implementation
{
    void SettingsWindow::NumberBox_Loaded(IInspectable const& sender, RoutedEventArgs const&)
    {
        const auto number_box = sender.as<Controls::NumberBox>();
        number_box.ApplyTemplate();
        disable_number_box_clear_button(number_box);
    }

    SettingsWindow::SettingsWindow()
    {
        InitializeComponent();
        update_animations_enabled_ = client_animations_enabled();
        update_progress_timer_ = DispatcherTimer();
        update_progress_timer_.Interval(std::chrono::milliseconds(33));
        const auto weak = get_weak();
        update_progress_timer_.Tick([weak](IInspectable const&, IInspectable const&) {
            if (const auto self = weak.get())
            {
                self->advance_update_progress();
            }
        });
        Closed([weak](IInspectable const&, WindowEventArgs const&) {
            if (const auto self = weak.get())
            {
                self->cancel_update_download();
            }
        });
        ApplyLocalizedResources();
        ApplyAppearancePreferences();
        configure_window();
        HWND window{};
        check_hresult(this->try_as<::IWindowNative>()->get_WindowHandle(&window));
        constexpr int logical_width = 820;
        constexpr int logical_height = 640;
        const UINT dpi = GetDpiForWindow(window);
        const int width = MulDiv(logical_width, dpi, 96);
        const int height = MulDiv(logical_height, dpi, 96);
        MONITORINFO monitor_info{ sizeof(monitor_info) };
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitor_info);
        const int x = monitor_info.rcWork.left + ((monitor_info.rcWork.right - monitor_info.rcWork.left) - width) / 2;
        const int y = monitor_info.rcWork.top + ((monitor_info.rcWork.bottom - monitor_info.rcWork.top) - height) / 2;
        SetWindowPos(window, nullptr, x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER);

        initializing_ = true;
        appearance_preferences_ = glance::app::load_appearance_preferences();
        LanguageComboBox().SelectedIndex(appearance_preferences_.language == L"zh-CN" ? 1 : 0);
        ThemeComboBox().SelectedIndex(static_cast<int>(appearance_preferences_.theme));
        AccentComboBox().SelectedIndex(static_cast<int>(appearance_preferences_.accent));
        LaunchAtSignInToggle().IsOn(launch_at_sign_in_enabled());
        update_preferences_ = glance::app::load_update_preferences();
        AutomaticUpdateCheckToggle().IsOn(update_preferences_.automatic_check_enabled);
        UpdateCheckFrequencyComboBox().SelectedIndex(
            static_cast<int>(update_preferences_.frequency));
        UpdateCheckFrequencyComboBox().IsEnabled(
            update_preferences_.automatic_check_enabled);
        DiagnosticsToggle().IsOn(glance::contracts::diagnostics_enabled());
        window_preferences_ = glance::app::load_window_preferences();
        DefaultWindowWidthNumberBox().Value(window_preferences_.default_width);
        DefaultWindowHeightNumberBox().Value(window_preferences_.default_height);
        RememberWindowSizeToggle().IsOn(window_preferences_.remember_size);
        AutoFitWindowSizeToggle().IsOn(window_preferences_.auto_fit_media);
        ShowAfterAutoFitToggle().IsOn(window_preferences_.show_after_auto_fit);
        DynamicAutoFitToggle().IsOn(window_preferences_.dynamic_auto_fit);
        AdaptiveMinimumPercentNumberBox().Value(window_preferences_.adaptive_minimum_percent);
        AdaptiveMaximumPercentNumberBox().Value(window_preferences_.adaptive_maximum_percent);
        AutoFitIgnoredExtensionsTextBox().Text(window_preferences_.auto_fit_ignored_extensions);
        RememberWindowPositionToggle().IsOn(window_preferences_.remember_position);
        WindowOpacityNumberBox().Value(window_preferences_.opacity_percent);
        update_auto_fit_controls_enabled();
        media_preview_preferences_ = glance::app::load_media_preview_preferences();
        DefaultAudioVolumeNumberBox().Value(media_preview_preferences_.audio_volume_percent);
        DefaultVideoVolumeNumberBox().Value(media_preview_preferences_.video_volume_percent);
        AutoplayAudioToggle().IsOn(media_preview_preferences_.autoplay_audio);
        AutoplayVideoToggle().IsOn(media_preview_preferences_.autoplay_video);
        ReverseSeekWheelToggle().IsOn(media_preview_preferences_.reverse_seek_wheel);
        MiddleClickGalleryModeToggle().IsOn(
            media_preview_preferences_.middle_click_gallery_mode);
        LoopGalleryScrollingToggle().IsOn(
            media_preview_preferences_.loop_gallery_scrolling);
        GallerySameExtensionOnlyToggle().IsOn(
            media_preview_preferences_.gallery_same_extension_only);
        ImageZoomMapToggle().IsOn(media_preview_preferences_.show_image_zoom_map);
        text_preferences_ = glance::app::load_text_preferences();
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
        FontSizeNumberBox().Value(text_preferences_.font_size);
        SyntaxHighlightingToggle().IsOn(text_preferences_.syntax_highlighting);
        SyntaxThemeComboBox().SelectedIndex(static_cast<int>(text_preferences_.syntax_theme));
        SyntaxThemeComboBox().IsEnabled(text_preferences_.syntax_highlighting);
        LineNumbersToggle().IsOn(text_preferences_.line_numbers);
        WordWrapToggle().IsOn(text_preferences_.word_wrap);
        path_copy_preferences_ = glance::app::load_path_copy_preferences();
        QuoteCopiedPathToggle().IsOn(path_copy_preferences_.quote_path);
        UnixPathSeparatorsToggle().IsOn(path_copy_preferences_.use_unix_separators);
        footer_preferences_ = glance::app::load_footer_preferences();
        rebuild_footer_field_rows();
        initializing_ = false;
        refresh_diagnostic_bundle_status();
        Activated([this](IInspectable const&, WindowActivatedEventArgs const& args) {
            if (args.WindowActivationState() != WindowActivationState::Deactivated)
            {
                refresh_launch_at_sign_in();
            }
        });
    }

    void SettingsWindow::InitializeSession(
        ExitCallback exit_callback,
        AppearanceChangedCallback appearance_changed_callback,
        TextPreferencesChangedCallback text_preferences_changed_callback,
        FooterPreferencesChangedCallback footer_preferences_changed_callback,
        WindowPreferencesChangedCallback window_preferences_changed_callback,
        ComponentChangedCallback component_changed_callback,
        SourceStatusRequestCallback source_status_request_callback,
        UpdateCheckCallback update_check_callback,
        NetworkDownloadCallback network_download_callback,
        UpdatePreferencesChangedCallback update_preferences_changed_callback)
    {
        exit_callback_ = std::move(exit_callback);
        appearance_changed_callback_ = std::move(appearance_changed_callback);
        text_preferences_changed_callback_ = std::move(text_preferences_changed_callback);
        footer_preferences_changed_callback_ = std::move(footer_preferences_changed_callback);
        window_preferences_changed_callback_ = std::move(window_preferences_changed_callback);
        component_changed_callback_ = std::move(component_changed_callback);
        source_status_request_callback_ = std::move(source_status_request_callback);
        update_check_callback_ = std::move(update_check_callback);
        network_download_callback_ = std::move(network_download_callback);
        update_preferences_changed_callback_ =
            std::move(update_preferences_changed_callback);
    }

    void SettingsWindow::configure_window()
    {
        HWND window{};
        check_hresult(this->try_as<::IWindowNative>()->get_WindowHandle(&window));
        if (const HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_GLANCE_APP)))
        {
            SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
            SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        }
        LONG_PTR window_style = GetWindowLongPtrW(window, GWL_STYLE);
        window_style &= ~(WS_SYSMENU | WS_MINIMIZEBOX);
        SetWindowLongPtrW(window, GWL_STYLE, window_style);
        if (const auto presenter = AppWindow().Presenter().try_as<Microsoft::UI::Windowing::OverlappedPresenter>())
        {
            presenter.IsMinimizable(false);
            presenter.IsMaximizable(true);
            presenter.SetBorderAndTitleBar(true, false);
        }
        ExtendsContentIntoTitleBar(true);
        SetTitleBar(SettingsTitleBarDragRegion());
        SetWindowPos(
            window,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void SettingsWindow::ApplyAppearancePreferences()
    {
        RootGrid().RequestedTheme(glance::app::element_theme(
            glance::app::load_appearance_preferences().theme));
    }

    void SettingsWindow::ShowAndActivate()
    {
        request_source_statuses();
        Activate();

        HWND window{};
        if (FAILED(this->try_as<::IWindowNative>()->get_WindowHandle(&window)) || window == nullptr)
        {
            return;
        }
        ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOW);
        SetWindowPos(
            window,
            HWND_TOP,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(window);
        SetActiveWindow(window);
    }

    void SettingsWindow::ShowComponentAction(
        std::wstring_view component_id,
        std::wstring_view action_id)
    {
        SettingsNavigation().SelectedItem(ComponentsNavigationItem());
        refresh_component_statuses();
        if (const auto action = glance::app::component_management_action(
                component_id,
                action_id,
                glance::app::current_ui_language()))
        {
            run_component_action(*action);
        }
    }

    void SettingsWindow::ShowUpdateDownload(glance::app::UpdateInstallerAsset asset)
    {
        SettingsNavigation().SelectedItem(AboutNavigationItem());
        download_and_install_update(std::move(asset));
    }

    void SettingsWindow::ApplyLocalizedResources()
    {
        const auto set_text = [](const auto& control, wchar_t const* key) {
            control.Text(glance::app::localize(key));
        };
        const auto set_content = [](const auto& control, wchar_t const* key) {
            control.Content(box_value(glance::app::localize(key)));
        };
        const auto set_tooltip = [](const auto& control, wchar_t const* key) {
            Controls::ToolTipService::SetToolTip(control, box_value(glance::app::localize(key)));
        };

        Title(glance::app::localize(L"SettingsTitle"));
        set_text(SettingsTitleText(), L"SettingsTitleText.Text");
        Controls::ToolTipService::SetToolTip(
            CloseSettingsButton(),
            box_value(glance::app::localize(L"CloseSettingsButton.ToolTipService.ToolTip")));
        set_content(GeneralNavigationItem(), L"GeneralNavigationItem.Content");
        set_content(WindowNavigationItem(), L"WindowNavigationItem.Content");
        set_content(FooterNavigationItem(), L"FooterNavigationItem.Content");
        set_content(TextPreviewNavigationItem(), L"TextPreviewNavigationItem.Content");
        set_content(MediaPreviewNavigationItem(), L"MediaPreviewNavigationItem.Content");
        set_content(ComponentsNavigationItem(), L"ComponentsNavigationItem.Content");
        set_content(MaintenanceNavigationItem(), L"MaintenanceNavigationItem.Content");
        set_content(AboutNavigationItem(), L"AboutNavigationItem.Content");
        set_text(ExitButtonText(), L"ExitButtonText.Text");
        set_text(GeneralPageTitle(), L"GeneralPageTitle.Text");
        set_text(GeneralPageDescription(), L"GeneralPageDescription.Text");
        set_text(LanguageLabel(), L"LanguageLabel.Text");
        set_content(EnglishLanguageItem(), L"EnglishLanguageItem.Content");
        set_content(ChineseLanguageItem(), L"ChineseLanguageItem.Content");
        set_text(ThemeLabel(), L"ThemeLabel.Text");
        const int selected_theme = ThemeComboBox().SelectedIndex();
        const bool was_initializing = initializing_;
        initializing_ = true;
        ThemeComboBox().Items().Clear();
        ThemeComboBox().Items().Append(box_value(glance::app::localize(L"ThemeSystemItem.Content")));
        ThemeComboBox().Items().Append(box_value(glance::app::localize(L"ThemeLightItem.Content")));
        ThemeComboBox().Items().Append(box_value(glance::app::localize(L"ThemeDarkItem.Content")));
        if (selected_theme >= 0)
        {
            ThemeComboBox().SelectedIndex(selected_theme);
        }
        initializing_ = was_initializing;
        set_text(AccentColorLabel(), L"AccentColorLabel.Text");
        set_text(AccentSystemText(), L"AccentSystemText.Text");
        set_text(AccentBlueText(), L"AccentBlueText.Text");
        set_text(AccentTealText(), L"AccentTealText.Text");
        set_text(AccentGreenText(), L"AccentGreenText.Text");
        set_text(AccentOrangeText(), L"AccentOrangeText.Text");
        set_text(AccentRedText(), L"AccentRedText.Text");
        set_text(AccentPinkText(), L"AccentPinkText.Text");
        set_text(AccentPurpleText(), L"AccentPurpleText.Text");
        set_text(LaunchTitle(), L"LaunchTitle.Text");
        set_text(LaunchDescription(), L"LaunchDescription.Text");
        set_text(UpdateGroupTitle(), L"UpdateGroupTitle.Text");
        set_text(AutomaticUpdateCheckLabel(), L"AutomaticUpdateCheckLabel.Text");
        set_text(
            AutomaticUpdateCheckDescription(),
            L"AutomaticUpdateCheckDescription.Text");
        set_text(UpdateCheckFrequencyLabel(), L"UpdateCheckFrequencyLabel.Text");
        set_content(UpdateFrequencyHourlyItem(), L"UpdateFrequencyHourlyItem.Content");
        set_content(UpdateFrequencyDailyItem(), L"UpdateFrequencyDailyItem.Content");
        set_content(UpdateFrequencyWeeklyItem(), L"UpdateFrequencyWeeklyItem.Content");
        set_content(UpdateFrequencyMonthlyItem(), L"UpdateFrequencyMonthlyItem.Content");
        set_text(FooterPageTitle(), L"FooterPageTitle.Text");
        set_text(FooterPageDescription(), L"FooterPageDescription.Text");
        set_text(FooterFieldsLabel(), L"FooterFieldsLabel.Text");
        set_content(FooterSizeCheckBox(), L"FooterSizeCheckBox.Content");
        set_content(FooterModifiedTimeCheckBox(), L"FooterModifiedTimeCheckBox.Content");
        set_content(FooterCreationTimeCheckBox(), L"FooterCreationTimeCheckBox.Content");
        set_content(FooterTakenTimeCheckBox(), L"FooterTakenTimeCheckBox.Content");
        set_content(FooterPermissionsCheckBox(), L"FooterPermissionsCheckBox.Content");
        set_content(FooterMediaInfoCheckBox(), L"FooterMediaInfoCheckBox.Content");
        for (const auto field : footer_preferences_.order)
        {
            const auto controls = footer_field_controls(field);
            set_tooltip(controls.move_up, L"FooterFieldMoveUpToolTip");
            set_tooltip(controls.move_down, L"FooterFieldMoveDownToolTip");
        }
        set_text(DiagnosticsTitle(), L"DiagnosticsTitle.Text");
        set_text(DiagnosticsDescription(), L"DiagnosticsDescription.Text");
        set_text(WindowPageTitle(), L"WindowPageTitle.Text");
        set_text(WindowPageDescription(), L"WindowPageDescription.Text");
        set_text(WindowBehaviorSectionTitle(), L"WindowBehaviorSectionTitle.Text");
        set_text(DefaultWindowSizeLabel(), L"DefaultWindowSizeLabel.Text");
        set_text(DefaultWindowSizeDescription(), L"DefaultWindowSizeDescription.Text");
        set_text(RememberWindowSizeLabel(), L"RememberWindowSizeLabel.Text");
        set_text(RememberWindowSizeDescription(), L"RememberWindowSizeDescription.Text");
        set_text(AutoFitWindowSizeLabel(), L"AutoFitWindowSizeLabel.Text");
        set_text(AutoFitWindowSizeDescription(), L"AutoFitWindowSizeDescription.Text");
        set_text(ShowAfterAutoFitLabel(), L"ShowAfterAutoFitLabel.Text");
        set_text(ShowAfterAutoFitDescription(), L"ShowAfterAutoFitDescription.Text");
        set_text(AdaptiveMediaSizeSectionTitle(), L"AdaptiveMediaSizeSectionTitle.Text");
        set_text(DynamicAutoFitLabel(), L"DynamicAutoFitLabel.Text");
        set_text(DynamicAutoFitDescription(), L"DynamicAutoFitDescription.Text");
        set_text(AdaptiveSizeRangeLabel(), L"AdaptiveSizeRangeLabel.Text");
        set_text(AdaptiveSizeRangeDescription(), L"AdaptiveSizeRangeDescription.Text");
        set_text(AutoFitIgnoredExtensionsLabel(), L"AutoFitIgnoredExtensionsLabel.Text");
        set_text(AutoFitIgnoredExtensionsDescription(), L"AutoFitIgnoredExtensionsDescription.Text");
        AutoFitIgnoredExtensionsTextBox().PlaceholderText(
            glance::app::localize(L"AutoFitIgnoredExtensionsTextBox.PlaceholderText"));
        set_content(ResetWindowSizesButton(), L"ResetWindowSizesButton.Content");
        set_text(RememberWindowPositionLabel(), L"RememberWindowPositionLabel.Text");
        set_text(RememberWindowPositionDescription(), L"RememberWindowPositionDescription.Text");
        set_content(ResetWindowPositionsButton(), L"ResetWindowPositionsButton.Content");
        set_text(WindowOpacityLabel(), L"WindowOpacityLabel.Text");
        set_text(WindowOpacityDescription(), L"WindowOpacityDescription.Text");
        set_text(MediaPreviewPageTitle(), L"MediaPreviewPageTitle.Text");
        set_text(MediaPreviewPageDescription(), L"MediaPreviewPageDescription.Text");
        set_text(MediaGeneralGroupTitle(), L"MediaGeneralGroupTitle.Text");
        set_text(MiddleClickGalleryModeLabel(), L"MiddleClickGalleryModeLabel.Text");
        set_text(
            MiddleClickGalleryModeDescription(),
            L"MiddleClickGalleryModeDescription.Text");
        set_text(LoopGalleryScrollingLabel(), L"LoopGalleryScrollingLabel.Text");
        set_text(
            LoopGalleryScrollingDescription(),
            L"LoopGalleryScrollingDescription.Text");
        set_text(GallerySameExtensionOnlyLabel(), L"GallerySameExtensionOnlyLabel.Text");
        set_text(
            GallerySameExtensionOnlyDescription(),
            L"GallerySameExtensionOnlyDescription.Text");
        set_text(ImagePreviewGroupTitle(), L"ImagePreviewGroupTitle.Text");
        set_text(ImageZoomMapLabel(), L"ImageZoomMapLabel.Text");
        set_text(ImageZoomMapDescription(), L"ImageZoomMapDescription.Text");
        set_text(AudioVideoPreviewGroupTitle(), L"AudioVideoPreviewGroupTitle.Text");
        set_text(DefaultAudioVolumeLabel(), L"DefaultAudioVolumeLabel.Text");
        set_text(DefaultAudioVolumeDescription(), L"DefaultAudioVolumeDescription.Text");
        set_text(DefaultVideoVolumeLabel(), L"DefaultVideoVolumeLabel.Text");
        set_text(DefaultVideoVolumeDescription(), L"DefaultVideoVolumeDescription.Text");
        set_text(AutoplayAudioLabel(), L"AutoplayAudioLabel.Text");
        set_text(AutoplayAudioDescription(), L"AutoplayAudioDescription.Text");
        set_text(AutoplayVideoLabel(), L"AutoplayVideoLabel.Text");
        set_text(AutoplayVideoDescription(), L"AutoplayVideoDescription.Text");
        set_text(ReverseSeekWheelLabel(), L"ReverseSeekWheelLabel.Text");
        set_text(ReverseSeekWheelDescription(), L"ReverseSeekWheelDescription.Text");
        set_text(TextPreviewPageTitle(), L"TextPreviewPageTitle.Text");
        set_text(TextPreviewPageDescription(), L"TextPreviewPageDescription.Text");
        set_text(PlainTextPreviewSectionTitle(), L"PlainTextPreviewSectionTitle.Text");
        set_text(FontFamilyLabel(), L"FontFamilyLabel.Text");
        set_text(FontFamilyDescription(), L"FontFamilyDescription.Text");
        set_text(FontSizeLabel(), L"FontSizeLabel.Text");
        set_text(FontSizeDescription(), L"FontSizeDescription.Text");
        set_text(SyntaxHighlightingLabel(), L"SyntaxHighlightingLabel.Text");
        set_text(SyntaxHighlightingDescription(), L"SyntaxHighlightingDescription.Text");
        set_text(SyntaxThemeLabel(), L"SyntaxThemeLabel.Text");
        set_text(SyntaxThemeDescription(), L"SyntaxThemeDescription.Text");
        const int selected_syntax_theme = SyntaxThemeComboBox().SelectedIndex();
        const bool was_initializing_syntax_theme = initializing_;
        initializing_ = true;
        SyntaxThemeComboBox().Items().Clear();
        static constexpr std::array syntax_theme_resources{
            std::wstring_view(L"SyntaxThemeGlance"),
            std::wstring_view(L"SyntaxThemeVisualStudio"),
            std::wstring_view(L"SyntaxThemeMonokai"),
            std::wstring_view(L"SyntaxThemeGitHub"),
            std::wstring_view(L"SyntaxThemeDracula"),
            std::wstring_view(L"SyntaxThemeSolarized"),
            std::wstring_view(L"SyntaxThemeNord"),
            std::wstring_view(L"SyntaxThemeOneDark"),
            std::wstring_view(L"SyntaxThemeGruvbox"),
            std::wstring_view(L"SyntaxThemeTomorrowNight"),
            std::wstring_view(L"SyntaxThemeCatppuccin"),
            std::wstring_view(L"SyntaxThemeTokyoNight"),
            std::wstring_view(L"SyntaxThemeRosePine"),
            std::wstring_view(L"SyntaxThemeEverforest"),
            std::wstring_view(L"SyntaxThemeAyu"),
            std::wstring_view(L"SyntaxThemeHorizon"),
            std::wstring_view(L"SyntaxThemePaperColor"),
            std::wstring_view(L"SyntaxThemeMaterial") };
        static_assert(
            syntax_theme_resources.size() ==
            static_cast<std::size_t>(glance::app::SyntaxThemePreference::material) + 1);
        for (const auto resource : syntax_theme_resources)
        {
            SyntaxThemeComboBox().Items().Append(box_value(glance::app::localize(resource)));
        }
        if (selected_syntax_theme >= 0)
        {
            SyntaxThemeComboBox().SelectedIndex(selected_syntax_theme);
        }
        initializing_ = was_initializing_syntax_theme;
        set_text(LineNumbersLabel(), L"LineNumbersLabel.Text");
        set_text(LineNumbersDescription(), L"LineNumbersDescription.Text");
        set_text(WordWrapLabel(), L"WordWrapLabel.Text");
        set_text(WordWrapDescription(), L"WordWrapDescription.Text");
        set_text(PathCopyGroupLabel(), L"PathCopyGroupLabel.Text");
        set_text(QuoteCopiedPathLabel(), L"QuoteCopiedPathLabel.Text");
        set_text(QuoteCopiedPathDescription(), L"QuoteCopiedPathDescription.Text");
        set_text(UnixPathSeparatorsLabel(), L"UnixPathSeparatorsLabel.Text");
        set_text(UnixPathSeparatorsDescription(), L"UnixPathSeparatorsDescription.Text");
        set_text(ComponentsPageTitle(), L"ComponentsPageTitle.Text");
        set_text(ComponentsPageDescription(), L"ComponentsPageDescription.Text");
        set_text(AppearanceGroupTitle(), L"AppearanceGroupTitle.Text");
        set_text(StartupGroupTitle(), L"StartupGroupTitle.Text");
        set_text(ComponentLocationGroupTitle(), L"ComponentLocationGroupTitle.Text");
        set_text(ComponentFolderLabel(), L"ComponentFolderLabel.Text");
        set_text(ComponentFolderDescription(), L"ComponentFolderDescription.Text");
        set_content(OpenComponentsFolderButton(), L"OpenComponentsFolderButton.Content");
        set_text(ComponentStatusGroupTitle(), L"ComponentStatusGroupTitle.Text");
        set_text(SourceLocationGroupTitle(), L"SourceLocationGroupTitle.Text");
        set_text(SourceFolderLabel(), L"SourceFolderLabel.Text");
        set_text(SourceFolderDescription(), L"SourceFolderDescription.Text");
        set_content(OpenSourcesFolderButton(), L"OpenSourcesFolderButton.Content");
        set_text(SourceStatusGroupTitle(), L"SourceStatusGroupTitle.Text");
        set_text(MaintenancePageTitle(), L"MaintenancePageTitle.Text");
        set_text(MaintenancePageDescription(), L"MaintenancePageDescription.Text");
        set_text(RuntimeStatusGroupTitle(), L"RuntimeStatusGroupTitle.Text");
        set_text(MaintenanceActionsGroupTitle(), L"MaintenanceActionsGroupTitle.Text");
        set_text(InputCoreLabel(), L"InputCoreLabel.Text");
        set_text(WebViewAvailabilityLabel(), L"WebViewAvailabilityLabel.Text");
        set_content(WebViewDownloadLink(), L"WebViewDownloadLink.Content");
        set_text(AdministratorAccessLabel(), L"AdministratorAccessLabel.Text");
        set_text(DiagnosticBundleLabel(), L"DiagnosticBundleLabel.Text");
        set_content(ExportDiagnosticBundleButton(), L"ExportDiagnosticBundleButton.Content");
        set_text(ResetAllSettingsLabel(), L"ResetAllSettingsLabel.Text");
        set_text(ResetAllSettingsDescription(), L"ResetAllSettingsDescription.Text");
        set_content(ResetAllSettingsButton(), L"ResetAllSettingsButton.Content");
        refresh_diagnostic_bundle_status();
        set_text(AboutPageTitle(), L"AboutPageTitle.Text");
        set_text(AboutPageDescription(), L"AboutPageDescription.Text");
        set_text(AboutAuthorLabel(), L"AboutAuthorLabel.Text");
        set_text(AboutLicenseText(), L"AboutLicenseText.Text");
        set_content(AboutProjectLink(), L"AboutProjectLink.Content");
        set_content(CheckForUpdatesButton(), L"CheckForUpdatesButton.Content");
        set_content(CancelUpdateButton(), L"Cancel");
        AboutVersionText().Text(glance::app::localize_format(
            L"VersionFormat", { GLANCE_VERSION_WSTRING }));
        refresh_runtime_statuses();
        refresh_component_statuses();
        request_source_statuses();
        rebuild_component_settings();
    }

    bool SettingsWindow::launch_at_sign_in_enabled() const
    {
        return glance::app::launch_at_sign_in_enabled();
    }

    void SettingsWindow::refresh_launch_at_sign_in()
    {
        const bool was_initializing = initializing_;
        initializing_ = true;
        LaunchAtSignInToggle().IsOn(launch_at_sign_in_enabled());
        initializing_ = was_initializing;
    }

    void SettingsWindow::set_launch_at_sign_in(bool enabled)
    {
        static_cast<void>(glance::app::set_launch_at_sign_in(enabled));
        refresh_launch_at_sign_in();
    }

    void SettingsWindow::refresh_runtime_statuses()
    {
        const bool core_running = named_mutex_exists(L"Local\\Glance.Core");
        const bool webview_available = glance::app::webview_runtime_available();
        set_status_indicator(
            CoreStatusIcon(),
            CoreStatusText(),
            core_running,
            L"CoreRunning",
            L"CoreNotRunning");
        set_status_indicator(
            WebViewAvailabilityStatusIcon(),
            WebViewAvailabilityStatusText(),
            webview_available,
            L"WebViewAvailable",
            L"WebViewUnavailable");
        WebViewDownloadLink().Visibility(
            webview_available ? Visibility::Collapsed : Visibility::Visible);
        set_status_indicator(
            AdministratorAccessStatusIcon(),
            AdministratorAccessStatusText(),
            core_running && named_mutex_exists(L"Local\\Glance.Core.Elevated"),
            L"AdministratorAccessAvailable",
            L"AdministratorAccessUnavailable");
    }

    void SettingsWindow::refresh_component_statuses()
    {
        ComponentStatusList().Children().Clear();
        const auto statuses =
            glance::app::component_statuses(glance::app::current_ui_language());
        ComponentEmptyState().Visibility(
            statuses.empty() ? Visibility::Visible : Visibility::Collapsed);
        ComponentStatusScroller().Visibility(
            statuses.empty() ? Visibility::Collapsed : Visibility::Visible);
        const auto row_style =
            SettingsNavigation().Resources()
                .Lookup(box_value(L"SettingsRowStyle"))
                .as<Style>();
        const auto detail_style =
            SettingsNavigation().Resources()
                .Lookup(box_value(L"SettingsDescriptionStyle"))
                .as<Style>();
        const auto divider_brush = Application::Current().Resources().TryLookup(
            box_value(L"DividerStrokeColorDefaultBrush")).try_as<Media::Brush>();
        for (std::size_t index = 0; index < statuses.size(); ++index)
        {
            const auto& status = statuses[index];
            if (index != 0 && divider_brush != nullptr)
            {
                Shapes::Rectangle divider;
                divider.Height(1);
                divider.Fill(divider_brush);
                ComponentStatusList().Children().Append(divider);
            }
            Controls::Grid row;
            row.Style(row_style);
            Controls::ColumnDefinition content_column;
            content_column.Width(GridLength{ 1, GridUnitType::Star });
            row.ColumnDefinitions().Append(content_column);
            Controls::ColumnDefinition icon_column;
            icon_column.Width(GridLengthHelper::Auto());
            row.ColumnDefinitions().Append(icon_column);

            Controls::StackPanel content;
            content.Spacing(3);
            Controls::TextBlock title;
            title.Text(status.display_name);
            content.Children().Append(title);
            if (!status.detail.empty())
            {
                Controls::TextBlock health_detail;
                health_detail.Style(detail_style);
                health_detail.Text(status.detail);
                health_detail.TextWrapping(TextWrapping::Wrap);
                content.Children().Append(health_detail);
            }
            row.Children().Append(content);

            std::wstring state_key;
            Windows::UI::Color color{ 255, 96, 94, 92 };
            std::wstring glyph = L"\xE946";
            switch (status.state)
            {
            case glance::app::ComponentState::healthy:
                state_key = L"ComponentStateHealthy";
                color = { 255, 16, 124, 16 };
                glyph = L"\xE8FB";
                break;
            case glance::app::ComponentState::warning:
                state_key = L"ComponentStateWarning";
                color = { 255, 157, 93, 0 };
                glyph = L"\xE7BA";
                break;
            case glance::app::ComponentState::error:
                state_key = L"ComponentStateError";
                color = { 255, 196, 43, 28 };
                glyph = L"\xE711";
                break;
            default:
                state_key = L"ComponentStateError";
                break;
            }

            Controls::StackPanel actions;
            actions.Orientation(Controls::Orientation::Horizontal);
            actions.Spacing(8);
            actions.VerticalAlignment(VerticalAlignment::Center);
            const auto weak = get_weak();
            for (const auto& action : status.actions)
            {
                Controls::Button button;
                button.Content(box_value(action.button_text));
                button.Click([weak, action](IInspectable const&, RoutedEventArgs const&) {
                    if (const auto self = weak.get())
                    {
                        self->run_component_action(action);
                    }
                });
                actions.Children().Append(button);
            }
            Controls::FontIcon status_icon;
            status_icon.Glyph(glyph);
            status_icon.FontSize(18);
            status_icon.Foreground(Media::SolidColorBrush(color));
            status_icon.VerticalAlignment(VerticalAlignment::Center);
            Controls::ToolTipService::SetToolTip(
                status_icon,
                box_value(glance::app::localize(state_key)));
            actions.Children().Append(status_icon);
            Controls::Grid::SetColumn(actions, 1);
            row.Children().Append(actions);
            ComponentStatusList().Children().Append(row);
        }
    }

    void SettingsWindow::request_source_statuses()
    {
        if (source_status_request_callback_)
        {
            static_cast<void>(source_status_request_callback_(
                winrt::to_string(glance::app::current_ui_language())));
        }
    }

    void SettingsWindow::HandleSourceStatuses(std::string_view payload)
    {
        using namespace winrt::Windows::Data::Json;
        try
        {
            const auto sources = JsonObject::Parse(winrt::to_hstring(payload))
                .GetNamedArray(L"sources");
            SourceStatusList().Children().Clear();
            SourceEmptyState().Visibility(
                sources.Size() == 0 ? Visibility::Visible : Visibility::Collapsed);
            SourceStatusScroller().Visibility(
                sources.Size() == 0 ? Visibility::Collapsed : Visibility::Visible);
            const auto row_style = SettingsNavigation().Resources()
                .Lookup(box_value(L"SettingsRowStyle")).as<Style>();
            const auto detail_style = SettingsNavigation().Resources()
                .Lookup(box_value(L"SettingsDescriptionStyle")).as<Style>();
            const auto divider_brush = Application::Current().Resources().TryLookup(
                box_value(L"DividerStrokeColorDefaultBrush")).try_as<Media::Brush>();
            for (std::uint32_t index = 0; index < sources.Size(); ++index)
            {
                const auto status = sources.GetObjectAt(index);
                if (index != 0 && divider_brush != nullptr)
                {
                    Shapes::Rectangle divider;
                    divider.Height(1);
                    divider.Fill(divider_brush);
                    SourceStatusList().Children().Append(divider);
                }
                Controls::Grid row;
                row.Style(row_style);
                Controls::ColumnDefinition content_column;
                content_column.Width(GridLength{ 1, GridUnitType::Star });
                row.ColumnDefinitions().Append(content_column);
                Controls::ColumnDefinition icon_column;
                icon_column.Width(GridLengthHelper::Auto());
                row.ColumnDefinitions().Append(icon_column);

                Controls::StackPanel content;
                content.Spacing(3);
                Controls::TextBlock title;
                title.Text(status.GetNamedString(L"name"));
                content.Children().Append(title);
                std::wstring detail = status.GetNamedString(L"detail", L"").c_str();
                if (detail.empty() &&
                    static_cast<std::uint32_t>(status.GetNamedNumber(L"code", 0)) == 1)
                {
                    detail = glance::app::localize(L"SourceLoadError");
                }
                if (!detail.empty())
                {
                    Controls::TextBlock detail_text;
                    detail_text.Style(detail_style);
                    detail_text.Text(detail);
                    detail_text.TextWrapping(TextWrapping::Wrap);
                    content.Children().Append(detail_text);
                }
                row.Children().Append(content);

                const auto severity = static_cast<std::uint32_t>(
                    status.GetNamedNumber(L"severity", 2));
                std::wstring state_key = L"ComponentStateError";
                Windows::UI::Color color{ 255, 196, 43, 28 };
                std::wstring glyph = L"\xE711";
                if (severity == 0)
                {
                    state_key = L"ComponentStateHealthy";
                    color = { 255, 16, 124, 16 };
                    glyph = L"\xE8FB";
                }
                else if (severity == 1)
                {
                    state_key = L"ComponentStateWarning";
                    color = { 255, 157, 93, 0 };
                    glyph = L"\xE7BA";
                }
                Controls::FontIcon icon;
                icon.Glyph(glyph);
                icon.FontSize(18);
                icon.Foreground(Media::SolidColorBrush(color));
                icon.VerticalAlignment(VerticalAlignment::Center);
                Controls::ToolTipService::SetToolTip(
                    icon, box_value(glance::app::localize(state_key)));
                Controls::Grid::SetColumn(icon, 1);
                row.Children().Append(icon);
                SourceStatusList().Children().Append(row);
            }
        }
        catch (...)
        {
        }
    }

    void SettingsWindow::rebuild_component_settings()
    {
        const bool was_initializing = initializing_;
        initializing_ = true;

        const auto panel = ComponentDocumentSettingsPanel();
        panel.Children().Clear();
        auto settings = glance::app::component_settings(
            glance::app::current_ui_language());
        std::erase_if(settings, [](const auto& setting) {
            return setting.page != glance::contracts::components::
                ComponentSettingPage::document_preview;
        });
        if (settings.empty())
        {
            initializing_ = was_initializing;
            return;
        }

        const auto title_style = SettingsNavigation().Resources()
            .Lookup(box_value(L"SettingsGroupTitleStyle")).as<Style>();
        const auto group_style = SettingsNavigation().Resources()
            .Lookup(box_value(L"SettingsGroupStyle")).as<Style>();
        const auto row_style = SettingsNavigation().Resources()
            .Lookup(box_value(L"SettingsRowStyle")).as<Style>();
        const auto description_style = SettingsNavigation().Resources()
            .Lookup(box_value(L"SettingsDescriptionStyle")).as<Style>();
        const auto divider_brush = Application::Current().Resources().TryLookup(
            box_value(L"DividerStrokeColorDefaultBrush")).try_as<Media::Brush>();
        const auto weak = get_weak();

        std::size_t index{};
        while (index < settings.size())
        {
            const auto group_component = settings[index].component_id;
            const auto group_id = settings[index].group_id;
            Controls::StackPanel group;
            group.Spacing(8);
            Controls::TextBlock title;
            title.Style(title_style);
            title.Text(settings[index].group_title);
            group.Children().Append(title);

            Controls::Border border;
            border.Style(group_style);
            Controls::StackPanel rows;
            bool first_row = true;
            while (index < settings.size() &&
                   settings[index].component_id == group_component &&
                   settings[index].group_id == group_id)
            {
                const auto setting = settings[index++];
                if (!first_row)
                {
                    Shapes::Rectangle divider;
                    divider.Height(1);
                    divider.Fill(divider_brush);
                    rows.Children().Append(divider);
                }
                first_row = false;

                Controls::Grid row;
                row.Style(row_style);
                Controls::ColumnDefinition content_column;
                content_column.Width(GridLength{ 1, GridUnitType::Star });
                row.ColumnDefinitions().Append(content_column);
                Controls::ColumnDefinition control_column;
                control_column.Width(
                    setting.kind == glance::contracts::components::
                        ComponentSettingKind::choice
                    ? GridLength{ 220, GridUnitType::Pixel }
                    : GridLengthHelper::Auto());
                row.ColumnDefinitions().Append(control_column);

                Controls::StackPanel content;
                content.Spacing(3);
                content.VerticalAlignment(VerticalAlignment::Center);
                Controls::TextBlock label;
                label.Text(setting.label);
                content.Children().Append(label);
                if (!setting.description.empty())
                {
                    Controls::TextBlock description;
                    description.Style(description_style);
                    description.Text(setting.description);
                    content.Children().Append(description);
                }
                row.Children().Append(content);

                const auto stored_value = glance::app::component_setting_value(
                    setting.component_id,
                    setting.setting_id,
                    setting.default_value);
                if (setting.kind == glance::contracts::components::
                        ComponentSettingKind::toggle)
                {
                    Controls::ToggleSwitch toggle;
                    toggle.IsOn(stored_value != 0);
                    Controls::Grid::SetColumn(toggle, 1);
                    toggle.Toggled([
                        weak,
                        component_id = setting.component_id,
                        setting_id = setting.setting_id](
                            IInspectable const& sender,
                            RoutedEventArgs const&) {
                        if (const auto self = weak.get();
                            self != nullptr && !self->initializing_)
                        {
                            glance::app::save_component_setting_value(
                                component_id,
                                setting_id,
                                sender.as<Controls::ToggleSwitch>().IsOn() ? 1 : 0);
                        }
                    });
                    row.Children().Append(toggle);
                }
                else
                {
                    Controls::ComboBox combo;
                    combo.HorizontalAlignment(HorizontalAlignment::Stretch);
                    combo.VerticalAlignment(VerticalAlignment::Center);
                    int selected_index = -1;
                    for (std::size_t option_index = 0;
                         option_index < setting.options.size();
                         ++option_index)
                    {
                        combo.Items().Append(box_value(setting.options[option_index].text));
                        if (setting.options[option_index].value == stored_value)
                        {
                            selected_index = static_cast<int>(option_index);
                        }
                    }
                    if (selected_index < 0)
                    {
                        const auto default_option = std::ranges::find_if(
                            setting.options,
                            [&setting](const auto& option) {
                                return option.value == setting.default_value;
                            });
                        selected_index = default_option == setting.options.end()
                            ? 0
                            : static_cast<int>(std::distance(
                                setting.options.begin(),
                                default_option));
                    }
                    combo.SelectedIndex(selected_index);
                    Controls::Grid::SetColumn(combo, 1);
                    combo.SelectionChanged([
                        weak,
                        component_id = setting.component_id,
                        setting_id = setting.setting_id,
                        options = setting.options](
                            IInspectable const& sender,
                            Controls::SelectionChangedEventArgs const&) {
                        const auto self = weak.get();
                        const int selected = sender.as<Controls::ComboBox>().SelectedIndex();
                        if (self == nullptr || self->initializing_ || selected < 0 ||
                            selected >= static_cast<int>(options.size()))
                        {
                            return;
                        }
                        glance::app::save_component_setting_value(
                            component_id,
                            setting_id,
                            options[static_cast<std::size_t>(selected)].value);
                    });
                    row.Children().Append(combo);
                }
                rows.Children().Append(row);
            }
            border.Child(rows);
            group.Children().Append(border);
            panel.Children().Append(group);
        }
        initializing_ = was_initializing;
    }

    void SettingsWindow::refresh_diagnostic_bundle_status()
    {
        switch (diagnostic_bundle_state_)
        {
        case DiagnosticBundleState::packaging:
            DiagnosticBundleStatusText().Text(glance::app::localize(L"DiagnosticBundlePackaging"));
            break;
        case DiagnosticBundleState::succeeded:
            DiagnosticBundleStatusText().Text(glance::app::localize_format(
                L"DiagnosticBundleCreated", { diagnostic_bundle_path_ }));
            break;
        case DiagnosticBundleState::failed:
            DiagnosticBundleStatusText().Text(glance::app::localize(L"DiagnosticBundleFailed"));
            break;
        default:
            DiagnosticBundleStatusText().Text(glance::app::localize(L"DiagnosticBundleDescription"));
            break;
        }
    }

    void SettingsWindow::LaunchAtSignInToggle_Toggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (!initializing_)
        {
            set_launch_at_sign_in(LaunchAtSignInToggle().IsOn());
        }
    }

    void SettingsWindow::AutomaticUpdateCheckToggle_Toggled(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        if (initializing_)
        {
            return;
        }
        update_preferences_.automatic_check_enabled = AutomaticUpdateCheckToggle().IsOn();
        update_preferences_.last_successful_check = 0;
        update_preferences_.retry_after = 0;
        update_preferences_.skipped_version.clear();
        UpdateCheckFrequencyComboBox().IsEnabled(
            update_preferences_.automatic_check_enabled);
        glance::app::save_update_preferences(update_preferences_);
        if (update_preferences_changed_callback_)
        {
            update_preferences_changed_callback_();
        }
    }

    void SettingsWindow::UpdateCheckFrequencyComboBox_SelectionChanged(
        IInspectable const&,
        Controls::SelectionChangedEventArgs const&)
    {
        if (initializing_)
        {
            return;
        }
        update_preferences_ = glance::app::load_update_preferences();
        update_preferences_.frequency = static_cast<glance::app::UpdateCheckFrequency>(
            std::clamp(UpdateCheckFrequencyComboBox().SelectedIndex(), 0, 3));
        glance::app::save_update_preferences(update_preferences_);
    }

    void SettingsWindow::DiagnosticsToggle_Toggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (!initializing_)
        {
            glance::contracts::set_diagnostics_enabled(DiagnosticsToggle().IsOn());
        }
    }

    void SettingsWindow::WindowPreferenceToggle_Toggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (!initializing_)
        {
            window_preferences_.remember_size = RememberWindowSizeToggle().IsOn();
            window_preferences_.auto_fit_media = AutoFitWindowSizeToggle().IsOn();
            window_preferences_.show_after_auto_fit = ShowAfterAutoFitToggle().IsOn();
            window_preferences_.dynamic_auto_fit = DynamicAutoFitToggle().IsOn();
            window_preferences_.remember_position = RememberWindowPositionToggle().IsOn();
            update_auto_fit_controls_enabled();
            glance::app::save_window_preferences(window_preferences_);
            if (window_preferences_changed_callback_)
            {
                window_preferences_changed_callback_();
            }
        }
    }

    void SettingsWindow::WindowNumberBox_ValueChanged(
        IInspectable const& sender,
        Controls::NumberBoxValueChangedEventArgs const&)
    {
        if (initializing_)
        {
            return;
        }

        const double width = DefaultWindowWidthNumberBox().Value();
        const double height = DefaultWindowHeightNumberBox().Value();
        const double opacity = WindowOpacityNumberBox().Value();
        const double adaptive_minimum = AdaptiveMinimumPercentNumberBox().Value();
        const double adaptive_maximum = AdaptiveMaximumPercentNumberBox().Value();
        if (!std::isfinite(width) || !std::isfinite(height) || !std::isfinite(opacity) ||
            !std::isfinite(adaptive_minimum) || !std::isfinite(adaptive_maximum))
        {
            initializing_ = true;
            DefaultWindowWidthNumberBox().Value(window_preferences_.default_width);
            DefaultWindowHeightNumberBox().Value(window_preferences_.default_height);
            WindowOpacityNumberBox().Value(window_preferences_.opacity_percent);
            AdaptiveMinimumPercentNumberBox().Value(window_preferences_.adaptive_minimum_percent);
            AdaptiveMaximumPercentNumberBox().Value(window_preferences_.adaptive_maximum_percent);
            initializing_ = false;
            return;
        }

        window_preferences_.default_width = static_cast<std::uint32_t>(
            std::clamp(std::lround(width), 480L, 7680L));
        window_preferences_.default_height = static_cast<std::uint32_t>(
            std::clamp(std::lround(height), 320L, 4320L));
        window_preferences_.opacity_percent = static_cast<std::uint32_t>(
            std::clamp(std::lround(opacity), 10L, 100L));
        auto minimum_percent = static_cast<std::uint32_t>(
            std::clamp(std::lround(adaptive_minimum), 10L, 100L));
        auto maximum_percent = static_cast<std::uint32_t>(
            std::clamp(std::lround(adaptive_maximum), 10L, 100L));
        if (minimum_percent > maximum_percent)
        {
            const auto control = sender.try_as<Controls::NumberBox>();
            if (control != nullptr && control.Name() == L"AdaptiveMinimumPercentNumberBox")
            {
                maximum_percent = minimum_percent;
            }
            else
            {
                minimum_percent = maximum_percent;
            }
            initializing_ = true;
            AdaptiveMinimumPercentNumberBox().Value(minimum_percent);
            AdaptiveMaximumPercentNumberBox().Value(maximum_percent);
            initializing_ = false;
        }
        window_preferences_.adaptive_minimum_percent = minimum_percent;
        window_preferences_.adaptive_maximum_percent = maximum_percent;
        glance::app::save_window_preferences(window_preferences_);
        if (window_preferences_changed_callback_)
        {
            window_preferences_changed_callback_();
        }
    }

    void SettingsWindow::AutoFitIgnoredExtensionsTextBox_TextChanged(
        IInspectable const&,
        Controls::TextChangedEventArgs const&)
    {
        if (initializing_)
        {
            return;
        }
        window_preferences_.auto_fit_ignored_extensions =
            AutoFitIgnoredExtensionsTextBox().Text().c_str();
        glance::app::save_window_preferences(window_preferences_);
        if (window_preferences_changed_callback_)
        {
            window_preferences_changed_callback_();
        }
    }

    void SettingsWindow::update_auto_fit_controls_enabled() noexcept
    {
        const bool enabled = AutoFitWindowSizeToggle().IsOn();
        ShowAfterAutoFitToggle().IsEnabled(enabled);
        DynamicAutoFitToggle().IsEnabled(enabled);
        AdaptiveMinimumPercentNumberBox().IsEnabled(enabled);
        AdaptiveMaximumPercentNumberBox().IsEnabled(enabled);
        AutoFitIgnoredExtensionsTextBox().IsEnabled(enabled);
    }

    void SettingsWindow::set_media_volume(
        Controls::NumberBox const& control,
        double value,
        std::uint32_t& destination)
    {
        if (initializing_)
        {
            return;
        }

        if (!std::isfinite(value))
        {
            initializing_ = true;
            control.Value(destination);
            initializing_ = false;
            return;
        }

        const auto volume = static_cast<std::uint32_t>(std::clamp(std::lround(value), 0L, 100L));
        destination = volume;
        if (std::abs(value - volume) > 0.001)
        {
            const bool was_initializing = initializing_;
            initializing_ = true;
            control.Value(volume);
            initializing_ = was_initializing;
        }
        glance::app::save_media_preview_preferences(media_preview_preferences_);
    }

    void SettingsWindow::DefaultAudioVolumeNumberBox_ValueChanged(
        IInspectable const&,
        Controls::NumberBoxValueChangedEventArgs const& args)
    {
        set_media_volume(
            DefaultAudioVolumeNumberBox(),
            args.NewValue(),
            media_preview_preferences_.audio_volume_percent);
    }

    void SettingsWindow::DefaultVideoVolumeNumberBox_ValueChanged(
        IInspectable const&,
        Controls::NumberBoxValueChangedEventArgs const& args)
    {
        set_media_volume(
            DefaultVideoVolumeNumberBox(),
            args.NewValue(),
            media_preview_preferences_.video_volume_percent);
    }

    void SettingsWindow::MediaPreferenceToggle_Toggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (initializing_)
        {
            return;
        }
        media_preview_preferences_.autoplay_audio = AutoplayAudioToggle().IsOn();
        media_preview_preferences_.autoplay_video = AutoplayVideoToggle().IsOn();
        media_preview_preferences_.reverse_seek_wheel = ReverseSeekWheelToggle().IsOn();
        media_preview_preferences_.middle_click_gallery_mode =
            MiddleClickGalleryModeToggle().IsOn();
        media_preview_preferences_.loop_gallery_scrolling =
            LoopGalleryScrollingToggle().IsOn();
        media_preview_preferences_.gallery_same_extension_only =
            GallerySameExtensionOnlyToggle().IsOn();
        media_preview_preferences_.show_image_zoom_map = ImageZoomMapToggle().IsOn();
        glance::app::save_media_preview_preferences(media_preview_preferences_);
    }

    fire_and_forget SettingsWindow::ExportDiagnosticBundleButton_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        const auto lifetime = get_strong();
        std::optional<std::filesystem::path> output_directory;
        try
        {
            HWND window{};
            check_hresult(this->try_as<::IWindowNative>()->get_WindowHandle(&window));
            output_directory = select_output_directory(
                window, glance::app::localize(L"DiagnosticBundlePickerTitle"));
        }
        catch (...)
        {
            diagnostic_bundle_state_ = DiagnosticBundleState::failed;
            refresh_diagnostic_bundle_status();
            co_return;
        }
        if (!output_directory)
        {
            co_return;
        }

        diagnostic_bundle_state_ = DiagnosticBundleState::packaging;
        diagnostic_bundle_path_.clear();
        ExportDiagnosticBundleButton().IsEnabled(false);
        refresh_diagnostic_bundle_status();
        const auto dispatcher = DispatcherQueue();
        co_await resume_background();
        bool succeeded{};
        std::wstring output_path;
        try
        {
            const auto result = create_diagnostic_bundle(*output_directory);
            succeeded = result.first;
            output_path = result.second;
        }
        catch (...)
        {
            succeeded = false;
            output_path.clear();
        }
        static_cast<void>(dispatcher.TryEnqueue([lifetime, succeeded, output_path] {
            lifetime->diagnostic_bundle_state_ = succeeded
                ? DiagnosticBundleState::succeeded
                : DiagnosticBundleState::failed;
            lifetime->diagnostic_bundle_path_ = output_path;
            lifetime->ExportDiagnosticBundleButton().IsEnabled(true);
            lifetime->refresh_diagnostic_bundle_status();
        }));
    }

    fire_and_forget SettingsWindow::ResetAllSettingsButton_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        const auto lifetime = get_strong();
        if (reset_confirmation_open_ || !exit_callback_)
        {
            co_return;
        }

        reset_confirmation_open_ = true;
        Controls::ContentDialog dialog;
        dialog.XamlRoot(RootGrid().XamlRoot());
        dialog.Title(box_value(glance::app::localize(L"ResetAllSettingsConfirmationTitle")));
        dialog.Content(box_value(glance::app::localize(L"ResetAllSettingsConfirmationMessage")));
        dialog.PrimaryButtonText(glance::app::localize(L"ResetAllSettingsConfirmationPrimary"));
        dialog.CloseButtonText(glance::app::localize(L"Cancel"));
        dialog.DefaultButton(Controls::ContentDialogButton::Close);
        const auto result = co_await dialog.ShowAsync();
        if (result != Controls::ContentDialogResult::Primary)
        {
            lifetime->reset_confirmation_open_ = false;
            co_return;
        }

        const LSTATUS registry_result = RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Glance");
        const bool registry_cleared = registry_result == ERROR_SUCCESS ||
            registry_result == ERROR_FILE_NOT_FOUND ||
            registry_result == ERROR_PATH_NOT_FOUND;
        if (registry_cleared)
        {
            lifetime->reset_confirmation_open_ = false;
            const auto callback = lifetime->exit_callback_;
            static_cast<void>(Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().TryEnqueue(
                [callback] { callback(); }));
            co_return;
        }

        Controls::ContentDialog failure_dialog;
        failure_dialog.XamlRoot(lifetime->RootGrid().XamlRoot());
        failure_dialog.Title(box_value(glance::app::localize(L"ResetAllSettingsFailedTitle")));
        failure_dialog.Content(box_value(glance::app::localize(L"ResetAllSettingsFailedMessage")));
        failure_dialog.CloseButtonText(glance::app::localize(L"OK"));
        co_await failure_dialog.ShowAsync();
        lifetime->reset_confirmation_open_ = false;
    }

    fire_and_forget SettingsWindow::CheckForUpdatesButton_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        const auto lifetime = get_strong();
        if (update_check_in_progress_)
        {
            co_return;
        }

        update_check_in_progress_ = true;
        CheckForUpdatesButton().IsEnabled(false);
        UpdateCheckProgressRing().Visibility(Visibility::Visible);
        UpdateCheckProgressRing().IsActive(true);

        apartment_context ui_thread;
        glance::app::UpdateCheckResult result;
        co_await resume_background();
        result = update_check_callback_ ? update_check_callback_() : glance::app::UpdateCheckResult{};
        co_await ui_thread;

        lifetime->update_check_in_progress_ = false;
        lifetime->CheckForUpdatesButton().IsEnabled(true);
        lifetime->UpdateCheckProgressRing().IsActive(false);
        lifetime->UpdateCheckProgressRing().Visibility(Visibility::Collapsed);
        if (lifetime->RootGrid().XamlRoot() == nullptr)
        {
            co_return;
        }

        const auto dialog_result = co_await ShowUpdateResultDialog(
            lifetime->RootGrid().XamlRoot(),
            result,
            false);
        if (dialog_result == static_cast<std::int32_t>(UpdatePromptResult::download))
        {
            lifetime->ShowUpdateDownload(std::move(result.installer));
        }
    }

    Windows::Foundation::IAsyncOperation<std::int32_t>
    SettingsWindow::ShowUpdateResultDialog(
        XamlRoot const& xaml_root,
        glance::app::UpdateCheckResult result,
        bool automatic_prompt)
    {
        try
        {
            if (xaml_root == nullptr)
            {
                co_return static_cast<std::int32_t>(UpdatePromptResult::failed);
            }

            Controls::ContentDialog dialog;
            dialog.XamlRoot(xaml_root);
            dialog.CloseButtonText(glance::app::localize(L"OK"));
            dialog.DefaultButton(Controls::ContentDialogButton::Close);
            const bool installer_available = static_cast<bool>(result.installer);

            switch (result.status)
            {
            case glance::app::UpdateCheckStatus::update_available:
                dialog.Title(box_value(glance::app::localize(L"UpdateAvailableTitle")));
                dialog.Content(box_value(glance::app::localize_format(
                    L"UpdateAvailableMessage", { result.latest_version })));
                dialog.PrimaryButtonText(glance::app::localize(
                    installer_available ? L"UpdateDownloadAndInstall" : L"UpdateOpenRelease"));
                if (installer_available)
                {
                    dialog.SecondaryButtonText(glance::app::localize(L"UpdateOpenRelease"));
                }
                dialog.CloseButtonText(glance::app::localize(
                    automatic_prompt ? L"UpdateSkipVersion" : L"Cancel"));
                dialog.DefaultButton(Controls::ContentDialogButton::Primary);
                break;
            case glance::app::UpdateCheckStatus::up_to_date:
                dialog.Title(box_value(glance::app::localize(L"UpdateUpToDateTitle")));
                dialog.Content(box_value(glance::app::localize_format(
                    L"UpdateUpToDateMessage", { GLANCE_VERSION_WSTRING })));
                break;
            case glance::app::UpdateCheckStatus::rate_limited:
                dialog.Title(box_value(glance::app::localize(L"UpdateCheckFailedTitle")));
                dialog.Content(box_value(glance::app::localize(L"UpdateRateLimitedMessage")));
                break;
            case glance::app::UpdateCheckStatus::no_release:
                dialog.Title(box_value(glance::app::localize(L"UpdateCheckFailedTitle")));
                dialog.Content(box_value(glance::app::localize(L"UpdateNoReleaseMessage")));
                break;
            default:
                dialog.Title(box_value(glance::app::localize(L"UpdateCheckFailedTitle")));
                dialog.Content(box_value(glance::app::localize(L"UpdateCheckFailedMessage")));
                break;
            }

            auto dialog_result = co_await dialog.ShowAsync();
            if (result.status == glance::app::UpdateCheckStatus::update_available)
            {
                if (installer_available &&
                    dialog_result == Controls::ContentDialogResult::Primary)
                {
                    co_return static_cast<std::int32_t>(UpdatePromptResult::download);
                }
                if ((!installer_available &&
                          dialog_result == Controls::ContentDialogResult::Primary) ||
                         dialog_result == Controls::ContentDialogResult::Secondary)
                {
                    static_cast<void>(co_await Windows::System::Launcher::LaunchUriAsync(
                        Windows::Foundation::Uri(safe_release_url(result.release_url))));
                    co_return static_cast<std::int32_t>(UpdatePromptResult::other);
                }
                if (automatic_prompt &&
                    dialog_result == Controls::ContentDialogResult::None)
                {
                    co_return static_cast<std::int32_t>(UpdatePromptResult::skip);
                }
            }
            co_return static_cast<std::int32_t>(UpdatePromptResult::other);
        }
        catch (...)
        {
            co_return static_cast<std::int32_t>(UpdatePromptResult::failed);
        }
    }

    fire_and_forget SettingsWindow::download_and_install_update(
        glance::app::UpdateInstallerAsset asset)
    {
        const auto lifetime = get_strong();
        if (update_download_in_progress_ || !asset)
        {
            co_return;
        }

        update_download_in_progress_ = true;
        update_installing_ = false;
        update_total_bytes_ = asset.size;
        const auto cancellation = std::make_shared<std::atomic_bool>(false);
        update_download_cancellation_ = cancellation;
        show_update_download_card(asset.version);

        const auto dispatcher = DispatcherQueue();
        const auto weak = get_weak();
        apartment_context ui_thread;
        glance::contracts::NetworkDownloadResult download_result;
        co_await resume_background();
        download_result = network_download_callback_
            ? network_download_callback_(
                glance::contracts::NetworkDownloadRequest{
                    asset.download_url,
                    asset.file_name,
                    asset.sha256,
                    asset.size },
                *cancellation,
                [dispatcher, weak, cancellation](std::uint64_t downloaded, std::uint64_t total) {
                static_cast<void>(dispatcher.TryEnqueue([weak, cancellation, downloaded, total] {
                    if (const auto self = weak.get();
                        self != nullptr &&
                        self->RootGrid().XamlRoot() != nullptr &&
                        self->update_download_cancellation_ == cancellation)
                    {
                        self->set_update_progress(downloaded, total);
                    }
                }));
                })
            : glance::contracts::NetworkDownloadResult{};
        co_await ui_thread;

        if (lifetime->RootGrid().XamlRoot() == nullptr)
        {
            lifetime->update_download_in_progress_ = false;
            co_return;
        }
        if (download_result.status == glance::contracts::NetworkDownloadStatus::cancelled)
        {
            lifetime->hide_update_card();
            co_return;
        }
        if (download_result.status != glance::contracts::NetworkDownloadStatus::succeeded)
        {
            const wchar_t* message_key = L"UpdateDownloadNetworkFailedMessage";
            if (download_result.status == glance::contracts::NetworkDownloadStatus::file_error)
            {
                message_key = L"UpdateDownloadFileFailedMessage";
            }
            else if (download_result.status == glance::contracts::NetworkDownloadStatus::integrity_error)
            {
                message_key = L"UpdateDownloadIntegrityFailedMessage";
            }
            lifetime->hide_update_card();

            try
            {
                Controls::ContentDialog dialog;
                dialog.XamlRoot(lifetime->RootGrid().XamlRoot());
                dialog.Title(box_value(glance::app::localize(L"UpdateDownloadFailedTitle")));
                dialog.Content(box_value(glance::app::localize(message_key)));
                dialog.PrimaryButtonText(glance::app::localize(L"UpdateRetry"));
                dialog.SecondaryButtonText(glance::app::localize(L"UpdateOpenRelease"));
                dialog.CloseButtonText(glance::app::localize(L"Cancel"));
                dialog.DefaultButton(Controls::ContentDialogButton::Primary);
                const auto result = co_await dialog.ShowAsync();
                if (result == Controls::ContentDialogResult::Primary)
                {
                    lifetime->download_and_install_update(std::move(asset));
                }
                else if (result == Controls::ContentDialogResult::Secondary)
                {
                    static_cast<void>(co_await Windows::System::Launcher::LaunchUriAsync(
                        Windows::Foundation::Uri(latest_release_url)));
                }
            }
            catch (...)
            {
            }
            co_return;
        }

        lifetime->set_update_progress(asset.size, asset.size);
        co_await resume_after(std::chrono::milliseconds(450));
        co_await ui_thread;
        if (lifetime->RootGrid().XamlRoot() == nullptr ||
            cancellation->load(std::memory_order_acquire))
        {
            co_return;
        }

        lifetime->show_update_installing_card();
        glance::app::UpdateLaunchStatus launch_status{};
        co_await resume_background();
        launch_status = glance::app::launch_update_installer(download_result.path);
        co_await ui_thread;
        if (launch_status == glance::app::UpdateLaunchStatus::launched ||
            lifetime->RootGrid().XamlRoot() == nullptr)
        {
            co_return;
        }

        lifetime->hide_update_card();
        try
        {
            Controls::ContentDialog dialog;
            dialog.XamlRoot(lifetime->RootGrid().XamlRoot());
            dialog.Title(box_value(glance::app::localize(
                launch_status == glance::app::UpdateLaunchStatus::cancelled
                    ? L"UpdateLaunchCancelledTitle"
                    : L"UpdateLaunchFailedTitle")));
            dialog.Content(box_value(glance::app::localize(
                launch_status == glance::app::UpdateLaunchStatus::cancelled
                    ? L"UpdateLaunchCancelledMessage"
                    : L"UpdateLaunchFailedMessage")));
            dialog.PrimaryButtonText(glance::app::localize(L"UpdateRetry"));
            dialog.SecondaryButtonText(glance::app::localize(L"UpdateOpenRelease"));
            dialog.CloseButtonText(glance::app::localize(L"Cancel"));
            dialog.DefaultButton(Controls::ContentDialogButton::Primary);
            const auto result = co_await dialog.ShowAsync();
            if (result == Controls::ContentDialogResult::Primary)
            {
                lifetime->download_and_install_update(std::move(asset));
            }
            else if (result == Controls::ContentDialogResult::Secondary)
            {
                static_cast<void>(co_await Windows::System::Launcher::LaunchUriAsync(
                    Windows::Foundation::Uri(latest_release_url)));
            }
        }
        catch (...)
        {
        }
    }

    fire_and_forget SettingsWindow::run_component_action(
        glance::app::ComponentManagementAction action)
    {
        const auto lifetime = get_strong();
        try
        {
            if (update_download_in_progress_ || action.lease == nullptr)
            {
                co_return;
            }

            const std::wstring language = glance::app::current_ui_language();
            const auto request = glance::app::prepare_component_management_action(action, language);
            if (!request)
            {
                Controls::ContentDialog dialog;
                dialog.XamlRoot(RootGrid().XamlRoot());
                dialog.Title(box_value(glance::app::localize(L"ComponentActionFailedTitle")));
                dialog.Content(box_value(glance::app::localize(L"ComponentActionFailedMessage")));
                dialog.CloseButtonText(glance::app::localize(L"OK"));
                co_await dialog.ShowAsync();
                co_return;
            }

            update_download_in_progress_ = true;
            update_installing_ = false;
            update_total_bytes_ = request.expected_size;
            const auto cancellation = std::make_shared<std::atomic_bool>(false);
            update_download_cancellation_ = cancellation;
            show_download_card(action.download_title, action.download_message);

            const auto dispatcher = DispatcherQueue();
            const auto weak = get_weak();
            apartment_context ui_thread;
            glance::contracts::NetworkDownloadResult download_result;
            co_await resume_background();
            download_result = network_download_callback_
                ? network_download_callback_(
                    glance::contracts::NetworkDownloadRequest{
                        request.url,
                        request.file_name,
                        request.sha256,
                        request.expected_size },
                    *cancellation,
                    [dispatcher, weak, cancellation](std::uint64_t downloaded, std::uint64_t total) {
                    static_cast<void>(
                        dispatcher.TryEnqueue([weak, cancellation, downloaded, total] {
                            if (const auto self = weak.get();
                                self != nullptr && self->RootGrid().XamlRoot() != nullptr &&
                                self->update_download_cancellation_ == cancellation)
                            {
                                self->set_update_progress(downloaded, total);
                            }
                        }));
                    })
                : glance::contracts::NetworkDownloadResult{};
            co_await ui_thread;

            if (lifetime->RootGrid().XamlRoot() == nullptr)
            {
                lifetime->update_download_in_progress_ = false;
                co_return;
            }
            if (download_result.status == glance::contracts::NetworkDownloadStatus::cancelled)
            {
                lifetime->hide_update_card();
                co_return;
            }
            if (download_result.status != glance::contracts::NetworkDownloadStatus::succeeded)
            {
                const wchar_t* message_key = L"ComponentActionDownloadNetworkFailedMessage";
                if (download_result.status == glance::contracts::NetworkDownloadStatus::file_error)
                {
                    message_key = L"ComponentActionDownloadFileFailedMessage";
                }
                else if (download_result.status == glance::contracts::NetworkDownloadStatus::integrity_error)
                {
                    message_key = L"ComponentActionDownloadIntegrityFailedMessage";
                }
                lifetime->hide_update_card();

                Controls::ContentDialog dialog;
                dialog.XamlRoot(lifetime->RootGrid().XamlRoot());
                dialog.Title(
                    box_value(glance::app::localize(L"ComponentActionDownloadFailedTitle")));
                dialog.Content(box_value(glance::app::localize(message_key)));
                dialog.PrimaryButtonText(glance::app::localize(L"UpdateRetry"));
                dialog.CloseButtonText(glance::app::localize(L"Cancel"));
                dialog.DefaultButton(Controls::ContentDialogButton::Primary);
                if (co_await dialog.ShowAsync() == Controls::ContentDialogResult::Primary)
                {
                    lifetime->run_component_action(std::move(action));
                }
                co_return;
            }

            lifetime->set_update_progress(request.expected_size, request.expected_size);
            co_await resume_after(std::chrono::milliseconds(350));
            co_await ui_thread;
            if (lifetime->RootGrid().XamlRoot() == nullptr)
            {
                lifetime->update_download_in_progress_ = false;
                co_return;
            }
            if (cancellation->load(std::memory_order_acquire))
            {
                lifetime->hide_update_card();
                co_return;
            }

            lifetime->show_preparing_card(action.preparing_title, action.preparing_message);
            glance::app::ComponentManagementActionCompletion completion;
            co_await resume_background();
            completion = glance::app::complete_component_management_action(
                action, download_result.path, language);
            std::error_code cleanup_error;
            std::filesystem::remove(download_result.path, cleanup_error);
            co_await ui_thread;

            if (lifetime->RootGrid().XamlRoot() == nullptr)
            {
                lifetime->update_download_in_progress_ = false;
                co_return;
            }
            if (!completion.succeeded)
            {
                lifetime->hide_update_card();
                Controls::ContentDialog dialog;
                dialog.XamlRoot(lifetime->RootGrid().XamlRoot());
                dialog.Title(box_value(glance::app::localize(L"ComponentActionFailedTitle")));
                dialog.Content(
                    box_value(completion.detail.empty()
                                  ? glance::app::localize(L"ComponentActionFailedMessage")
                                  : completion.detail));
                dialog.CloseButtonText(glance::app::localize(L"OK"));
                co_await dialog.ShowAsync();
                co_return;
            }

            lifetime->refresh_component_statuses();
            if (lifetime->component_changed_callback_)
            {
                lifetime->component_changed_callback_();
            }
            lifetime->update_installing_ = false;
            lifetime->UpdateProgressRing().IsIndeterminate(false);
            lifetime->UpdateProgressRing().Value(100);
            lifetime->UpdateProgressPercentText().Text(L"100%");
            lifetime->UpdateProgressPercentText().Visibility(Visibility::Visible);
            lifetime->UpdateProgressBytesText().Visibility(Visibility::Collapsed);
            lifetime->CancelUpdateButton().Visibility(Visibility::Collapsed);
            lifetime->UpdateCardTitle().Text(action.completed_title);
            lifetime->UpdateCardMessage().Text(action.completed_message);
            co_await resume_after(std::chrono::milliseconds(900));
            co_await ui_thread;
            if (lifetime->RootGrid().XamlRoot() != nullptr)
            {
                lifetime->hide_update_card();
            }
        }
        catch (...)
        {
            try
            {
                if (lifetime->RootGrid().XamlRoot() != nullptr &&
                    lifetime->update_download_in_progress_)
                {
                    lifetime->hide_update_card();
                }
            }
            catch (...)
            {
            }
        }
    }

    void SettingsWindow::show_update_download_card(std::wstring_view version)
    {
        show_download_card(
            glance::app::localize(L"UpdateDownloadTitle"),
            glance::app::localize_format(L"UpdateDownloadMessage", { version }));
    }

    void SettingsWindow::show_download_card(
        std::wstring_view title,
        std::wstring_view message)
    {
        update_displayed_progress_ = 0;
        update_start_progress_ = 0;
        update_target_progress_ = 0;
        update_animation_started_ms_ = GetTickCount64();
        SettingsNavigation().IsEnabled(false);
        UpdateOverlay().Visibility(Visibility::Visible);
        UpdateProgressRing().IsActive(true);
        UpdateProgressRing().IsIndeterminate(false);
        UpdateProgressRing().Value(0);
        UpdateProgressPercentText().Text(L"0%");
        UpdateProgressPercentText().Visibility(Visibility::Visible);
        UpdateCardTitle().Text(title);
        UpdateCardMessage().Text(message);
        UpdateProgressBytesText().Text(glance::app::localize_format(
            L"UpdateDownloadBytesFormat",
            { format_megabytes(0), format_megabytes(update_total_bytes_) }));
        UpdateProgressBytesText().Visibility(Visibility::Visible);
        CancelUpdateButton().Content(box_value(glance::app::localize(L"Cancel")));
        CancelUpdateButton().IsEnabled(true);
        CancelUpdateButton().Visibility(Visibility::Visible);
    }

    void SettingsWindow::set_update_progress(std::uint64_t downloaded, std::uint64_t total)
    {
        if (!update_download_in_progress_ || update_installing_ || total == 0)
        {
            return;
        }

        downloaded = std::min(downloaded, total);
        update_total_bytes_ = total;
        UpdateProgressBytesText().Text(glance::app::localize_format(
            L"UpdateDownloadBytesFormat",
            { format_megabytes(downloaded), format_megabytes(total) }));
        const double target = std::max(
            update_target_progress_,
            static_cast<double>(downloaded) * 100.0 / static_cast<double>(total));
        if (!update_animations_enabled_)
        {
            update_displayed_progress_ = target;
            update_target_progress_ = target;
            UpdateProgressRing().Value(target);
            UpdateProgressPercentText().Text(
                std::to_wstring(static_cast<int>(std::floor(target))) + L"%");
            return;
        }

        if (update_progress_timer_.IsEnabled())
        {
            advance_update_progress();
        }
        update_start_progress_ = update_displayed_progress_;
        update_target_progress_ = target;
        update_animation_started_ms_ = GetTickCount64();
        if (!update_progress_timer_.IsEnabled())
        {
            update_progress_timer_.Start();
        }
    }

    void SettingsWindow::advance_update_progress()
    {
        if (!update_download_in_progress_ || update_installing_)
        {
            update_progress_timer_.Stop();
            return;
        }

        constexpr double animation_duration_ms = 400.0;
        const double elapsed = static_cast<double>(GetTickCount64() - update_animation_started_ms_);
        const double progress = std::clamp(elapsed / animation_duration_ms, 0.0, 1.0);
        const double eased = 1.0 - std::pow(1.0 - progress, 3.0);
        update_displayed_progress_ = update_start_progress_ +
            (update_target_progress_ - update_start_progress_) * eased;
        UpdateProgressRing().Value(update_displayed_progress_);
        const int percentage = progress >= 1.0 && update_target_progress_ >= 100.0
            ? 100
            : static_cast<int>(std::floor(update_displayed_progress_));
        UpdateProgressPercentText().Text(std::to_wstring(percentage) + L"%");
        if (progress >= 1.0)
        {
            update_progress_timer_.Stop();
        }
    }

    void SettingsWindow::show_update_installing_card()
    {
        show_preparing_card(
            glance::app::localize(L"UpdateInstallingTitle"),
            glance::app::localize(L"UpdateInstallingMessage"));
    }

    void SettingsWindow::show_preparing_card(
        std::wstring_view title,
        std::wstring_view message)
    {
        update_installing_ = true;
        update_progress_timer_.Stop();
        UpdateProgressRing().IsIndeterminate(true);
        UpdateProgressPercentText().Visibility(Visibility::Collapsed);
        UpdateProgressBytesText().Visibility(Visibility::Collapsed);
        CancelUpdateButton().Visibility(Visibility::Collapsed);
        UpdateCardTitle().Text(title);
        UpdateCardMessage().Text(message);
    }

    void SettingsWindow::cancel_update_download()
    {
        if (!update_download_in_progress_ || update_installing_ || !update_download_cancellation_)
        {
            return;
        }

        update_download_cancellation_->store(true, std::memory_order_release);
        update_progress_timer_.Stop();
        UpdateProgressRing().IsIndeterminate(true);
        UpdateProgressPercentText().Visibility(Visibility::Collapsed);
        UpdateProgressBytesText().Visibility(Visibility::Collapsed);
        CancelUpdateButton().Visibility(Visibility::Collapsed);
        UpdateCardTitle().Text(glance::app::localize(L"UpdateCancellingTitle"));
        UpdateCardMessage().Text(glance::app::localize(L"UpdateCancellingMessage"));
    }

    void SettingsWindow::hide_update_card()
    {
        update_progress_timer_.Stop();
        update_download_in_progress_ = false;
        update_installing_ = false;
        update_download_cancellation_.reset();
        update_total_bytes_ = 0;
        UpdateProgressRing().IsActive(false);
        UpdateOverlay().Visibility(Visibility::Collapsed);
        SettingsNavigation().IsEnabled(true);
    }

    void SettingsWindow::CancelUpdateButton_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        cancel_update_download();
    }

    void SettingsWindow::OpenComponentsFolderButton_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        try
        {
            const auto path = glance::app::application_component_root();
            std::filesystem::create_directories(path);
            static_cast<void>(ShellExecuteW(
                nullptr,
                L"open",
                path.c_str(),
                nullptr,
                nullptr,
                SW_SHOWNORMAL));
        }
        catch (...)
        {
        }
    }

    void SettingsWindow::OpenSourcesFolderButton_Click(
        IInspectable const&,
        RoutedEventArgs const&)
    {
        try
        {
            const auto path = glance::app::application_component_root().parent_path() / L"sources";
            std::filesystem::create_directories(path);
            static_cast<void>(ShellExecuteW(
                nullptr,
                L"open",
                path.c_str(),
                nullptr,
                nullptr,
                SW_SHOWNORMAL));
        }
        catch (...)
        {
        }
    }

    void SettingsWindow::ResetWindowSizesButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        WindowResetStatusText().Text(
            glance::app::clear_window_sizes()
                ? glance::app::localize(L"SizesReset")
                : glance::app::localize(L"SizesResetFailed"));
    }

    void SettingsWindow::ResetWindowPositionsButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        WindowResetStatusText().Text(
            glance::app::clear_window_positions()
                ? glance::app::localize(L"PositionsReset")
                : glance::app::localize(L"PositionsResetFailed"));
    }

    void SettingsWindow::save_text_preferences()
    {
        if (initializing_)
        {
            return;
        }
        if (FontFamilyComboBox().SelectedItem() != nullptr)
        {
            text_preferences_.font_family = unbox_value<hstring>(FontFamilyComboBox().SelectedItem());
        }
        if (std::isfinite(FontSizeNumberBox().Value()))
        {
            text_preferences_.font_size = FontSizeNumberBox().Value();
        }
        text_preferences_.syntax_highlighting = SyntaxHighlightingToggle().IsOn();
        const int syntax_theme = SyntaxThemeComboBox().SelectedIndex();
        if (syntax_theme >= 0 && syntax_theme <= static_cast<int>(glance::app::SyntaxThemePreference::material))
        {
            text_preferences_.syntax_theme = static_cast<glance::app::SyntaxThemePreference>(syntax_theme);
        }
        SyntaxThemeComboBox().IsEnabled(text_preferences_.syntax_highlighting);
        text_preferences_.line_numbers = LineNumbersToggle().IsOn();
        text_preferences_.word_wrap = WordWrapToggle().IsOn();
        glance::app::save_text_preferences(text_preferences_);
        if (text_preferences_changed_callback_)
        {
            text_preferences_changed_callback_();
        }
    }

    void SettingsWindow::FontFamilyComboBox_SelectionChanged(
        IInspectable const&,
        Controls::SelectionChangedEventArgs const&)
    {
        save_text_preferences();
    }

    void SettingsWindow::FontSizeNumberBox_ValueChanged(
        IInspectable const&,
        Controls::NumberBoxValueChangedEventArgs const& args)
    {
        if (initializing_)
        {
            return;
        }

        if (!std::isfinite(args.NewValue()))
        {
            initializing_ = true;
            FontSizeNumberBox().Value(text_preferences_.font_size);
            initializing_ = false;
            return;
        }

        text_preferences_.font_size = args.NewValue();
        save_text_preferences();
    }

    void SettingsWindow::SyntaxThemeComboBox_SelectionChanged(
        IInspectable const&,
        Controls::SelectionChangedEventArgs const&)
    {
        save_text_preferences();
    }

    void SettingsWindow::TextPreferenceToggle_Toggled(IInspectable const&, RoutedEventArgs const&)
    {
        save_text_preferences();
    }

    void SettingsWindow::PathCopyPreferenceToggle_Toggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (initializing_)
        {
            return;
        }
        path_copy_preferences_.quote_path = QuoteCopiedPathToggle().IsOn();
        path_copy_preferences_.use_unix_separators = UnixPathSeparatorsToggle().IsOn();
        glance::app::save_path_copy_preferences(path_copy_preferences_);
    }

    SettingsWindow::FooterFieldControls SettingsWindow::footer_field_controls(
        glance::app::FooterField field)
    {
        using glance::app::FooterField;
        switch (field)
        {
        case FooterField::size:
            return { FooterSizeRow(), FooterSizeCheckBox(), FooterSizeMoveUpButton(), FooterSizeMoveDownButton() };
        case FooterField::modified_time:
            return { FooterModifiedTimeRow(), FooterModifiedTimeCheckBox(), FooterModifiedTimeMoveUpButton(), FooterModifiedTimeMoveDownButton() };
        case FooterField::creation_time:
            return { FooterCreationTimeRow(), FooterCreationTimeCheckBox(), FooterCreationTimeMoveUpButton(), FooterCreationTimeMoveDownButton() };
        case FooterField::taken_time:
            return { FooterTakenTimeRow(), FooterTakenTimeCheckBox(), FooterTakenTimeMoveUpButton(), FooterTakenTimeMoveDownButton() };
        case FooterField::permissions:
            return { FooterPermissionsRow(), FooterPermissionsCheckBox(), FooterPermissionsMoveUpButton(), FooterPermissionsMoveDownButton() };
        case FooterField::media_info:
            return { FooterMediaInfoRow(), FooterMediaInfoCheckBox(), FooterMediaInfoMoveUpButton(), FooterMediaInfoMoveDownButton() };
        default:
            return {};
        }
    }

    void SettingsWindow::rebuild_footer_field_rows()
    {
        const bool was_initializing = initializing_;
        initializing_ = true;
        auto children = FooterFieldsPanel().Children();
        children.Clear();
        for (std::size_t index = 0; index < footer_preferences_.order.size(); ++index)
        {
            const auto controls = footer_field_controls(footer_preferences_.order[index]);
            controls.checkbox.IsChecked(glance::app::footer_field_enabled(
                footer_preferences_, footer_preferences_.order[index]));
            controls.move_up.IsEnabled(index > 0);
            controls.move_down.IsEnabled(index + 1 < footer_preferences_.order.size());
            controls.row.BorderThickness(Thickness{
                0.0,
                0.0,
                0.0,
                index + 1 < footer_preferences_.order.size() ? 1.0 : 0.0 });
            children.Append(controls.row);
        }
        initializing_ = was_initializing;
    }

    void SettingsWindow::save_footer_preferences()
    {
        if (initializing_)
        {
            return;
        }
        glance::app::save_footer_preferences(footer_preferences_);
        if (footer_preferences_changed_callback_)
        {
            footer_preferences_changed_callback_();
        }
    }

    void SettingsWindow::FooterFieldCheckBox_Click(
        IInspectable const& sender,
        RoutedEventArgs const&)
    {
        if (initializing_)
        {
            return;
        }
        const auto checkbox = sender.try_as<Controls::CheckBox>();
        const auto field = checkbox == nullptr
            ? std::nullopt
            : footer_field_from_tag(checkbox.Tag());
        if (!field.has_value())
        {
            return;
        }
        const auto bit = glance::app::footer_field_bit(*field);
        if (checkbox.IsChecked().Value())
        {
            footer_preferences_.enabled_mask |= bit;
        }
        else
        {
            footer_preferences_.enabled_mask &= ~bit;
        }
        save_footer_preferences();
    }

    void SettingsWindow::FooterFieldMoveUpButton_Click(
        IInspectable const& sender,
        RoutedEventArgs const&)
    {
        const auto button = sender.try_as<Controls::Button>();
        const auto field = button == nullptr ? std::nullopt : footer_field_from_tag(button.Tag());
        if (!field.has_value())
        {
            return;
        }
        const auto position = std::ranges::find(footer_preferences_.order, *field);
        if (position == footer_preferences_.order.end() || position == footer_preferences_.order.begin())
        {
            return;
        }
        std::iter_swap(position, position - 1);
        rebuild_footer_field_rows();
        save_footer_preferences();
    }

    void SettingsWindow::FooterFieldMoveDownButton_Click(
        IInspectable const& sender,
        RoutedEventArgs const&)
    {
        const auto button = sender.try_as<Controls::Button>();
        const auto field = button == nullptr ? std::nullopt : footer_field_from_tag(button.Tag());
        if (!field.has_value())
        {
            return;
        }
        const auto position = std::ranges::find(footer_preferences_.order, *field);
        if (position == footer_preferences_.order.end() || position + 1 == footer_preferences_.order.end())
        {
            return;
        }
        std::iter_swap(position, position + 1);
        rebuild_footer_field_rows();
        save_footer_preferences();
    }

    void SettingsWindow::save_appearance_preferences()
    {
        if (initializing_)
        {
            return;
        }
        const auto language_item = LanguageComboBox().SelectedItem().try_as<Controls::ComboBoxItem>();
        const auto language_tag = language_item == nullptr
            ? hstring(L"en-US")
            : unbox_value_or<hstring>(language_item.Tag(), L"en-US");
        appearance_preferences_.language = glance::app::resolve_ui_language(language_tag.c_str());
        appearance_preferences_.theme = static_cast<glance::app::ThemePreference>(
            std::clamp(ThemeComboBox().SelectedIndex(), 0, 2));
        appearance_preferences_.accent = static_cast<glance::app::AccentPreference>(
            std::clamp(AccentComboBox().SelectedIndex(), 0, 7));
        glance::app::save_appearance_preferences(appearance_preferences_);
        glance::app::apply_ui_language(appearance_preferences_.language);
        glance::app::apply_accent_resources(appearance_preferences_);
        ApplyLocalizedResources();
        ApplyAppearancePreferences();
        if (appearance_changed_callback_)
        {
            appearance_changed_callback_();
        }
    }

    void SettingsWindow::AppearanceComboBox_SelectionChanged(
        IInspectable const&,
        Controls::SelectionChangedEventArgs const&)
    {
        save_appearance_preferences();
    }

    void SettingsWindow::SettingsNavigation_SelectionChanged(
        Controls::NavigationView const&,
        Controls::NavigationViewSelectionChangedEventArgs const& args)
    {
        const auto selected = args.SelectedItemContainer();
        const hstring tag = selected == nullptr
            ? L"general"
            : unbox_value_or<hstring>(selected.Tag(), L"general");
        GeneralSettingsPanel().Visibility(tag == L"general" ? Visibility::Visible : Visibility::Collapsed);
        WindowSettingsPanel().Visibility(tag == L"window" ? Visibility::Visible : Visibility::Collapsed);
        FooterSettingsPanel().Visibility(tag == L"footer" ? Visibility::Visible : Visibility::Collapsed);
        TextPreviewSettingsPanel().Visibility(tag == L"text" ? Visibility::Visible : Visibility::Collapsed);
        MediaPreviewSettingsPanel().Visibility(tag == L"media" ? Visibility::Visible : Visibility::Collapsed);
        ComponentsSettingsPanel().Visibility(tag == L"components" ? Visibility::Visible : Visibility::Collapsed);
        MaintenanceSettingsPanel().Visibility(tag == L"maintenance" ? Visibility::Visible : Visibility::Collapsed);
        AboutSettingsPanel().Visibility(tag == L"about" ? Visibility::Visible : Visibility::Collapsed);
        if (tag == L"general")
        {
            refresh_launch_at_sign_in();
        }
        if (tag == L"maintenance")
        {
            glance::app::refresh_webview_availability();
            refresh_runtime_statuses();
        }
        if (tag == L"components")
        {
            refresh_component_statuses();
            request_source_statuses();
        }
    }

    fire_and_forget SettingsWindow::ExitButton_Tapped(
        IInspectable const&,
        Input::TappedRoutedEventArgs const&)
    {
        ConfirmExit();
        co_return;
    }

    fire_and_forget SettingsWindow::ConfirmExit()
    {
        const auto lifetime = get_strong();
        if (exit_confirmation_open_)
        {
            co_return;
        }
        exit_confirmation_open_ = true;
        Controls::ContentDialog dialog;
        dialog.XamlRoot(RootGrid().XamlRoot());
        dialog.Title(box_value(glance::app::localize(L"ExitConfirmationTitle")));
        dialog.Content(box_value(glance::app::localize(L"ExitConfirmationMessage")));
        dialog.PrimaryButtonText(glance::app::localize(L"ExitConfirmationPrimary"));
        dialog.CloseButtonText(glance::app::localize(L"Cancel"));
        dialog.DefaultButton(Controls::ContentDialogButton::Close);
        const auto result = co_await dialog.ShowAsync();
        lifetime->exit_confirmation_open_ = false;
        if (result == Controls::ContentDialogResult::Primary && lifetime->exit_callback_)
        {
            const auto callback = lifetime->exit_callback_;
            static_cast<void>(Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().TryEnqueue(
                [callback] { callback(); }));
        }
    }

    void SettingsWindow::CloseSettingsButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        cancel_update_download();
        Close();
    }
}

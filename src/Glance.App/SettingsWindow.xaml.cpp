#include "pch.h"
#include "SettingsWindow.xaml.h"
#include "appearance_preferences.h"
#include "footer_preferences.h"
#include "localization.h"
#include "media_metadata_provider.h"
#include "office_availability.h"
#include "office_preview_cache.h"
#include "office_preview_preferences.h"
#include "path_copy_preferences.h"
#include "resource.h"
#include "startup_registration.h"
#include "text_preferences.h"
#include "update_checker.h"
#include "webview_availability.h"
#include "window_size_store.h"
#include "glance/contracts/diagnostics.h"
#include "../version.h"
#if __has_include("SettingsWindow.g.cpp")
#include "SettingsWindow.g.cpp"
#endif

#include <microsoft.ui.xaml.window.h>
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

namespace
{
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
        if (tag == L"permissions") return glance::app::FooterField::permissions;
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
        OfficeCacheCapacitySlider().ValueChanged(
            { this, &SettingsWindow::OfficeCacheCapacitySlider_ValueChanged });
        appearance_preferences_ = glance::app::load_appearance_preferences();
        LanguageComboBox().SelectedIndex(appearance_preferences_.language == L"zh-CN" ? 1 : 0);
        ThemeComboBox().SelectedIndex(static_cast<int>(appearance_preferences_.theme));
        AccentComboBox().SelectedIndex(static_cast<int>(appearance_preferences_.accent));
        LaunchAtSignInToggle().IsOn(launch_at_sign_in_enabled());
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
        text_preferences_ = glance::app::load_text_preferences();
        const auto font_families = glance::app::system_font_families();
        int selected_font{};
        for (std::size_t index = 0; index < font_families.size(); ++index)
        {
            FontFamilyComboBox().Items().Append(box_value(font_families[index]));
            if (_wcsicmp(font_families[index].c_str(), text_preferences_.font_family.c_str()) == 0)
            {
                selected_font = static_cast<int>(index);
            }
        }
        FontFamilyComboBox().SelectedIndex(selected_font);
        FontSizeNumberBox().Value(text_preferences_.font_size);
        SyntaxHighlightingToggle().IsOn(text_preferences_.syntax_highlighting);
        SyntaxThemeComboBox().SelectedIndex(static_cast<int>(text_preferences_.syntax_theme));
        SyntaxThemeComboBox().IsEnabled(text_preferences_.syntax_highlighting);
        LineNumbersToggle().IsOn(text_preferences_.line_numbers);
        WordWrapToggle().IsOn(text_preferences_.word_wrap);
        office_preview_preferences_ = glance::app::load_office_preview_preferences();
        OfficeCacheCapacitySlider().Value(office_preview_preferences_.cache_capacity);
        OfficeCacheCapacityValueText().Text(
            std::to_wstring(office_preview_preferences_.cache_capacity));
        OfficeCacheExpirationNumberBox().Value(
            office_preview_preferences_.cache_expiration_minutes);
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
        WindowPreferencesChangedCallback window_preferences_changed_callback)
    {
        exit_callback_ = std::move(exit_callback);
        appearance_changed_callback_ = std::move(appearance_changed_callback);
        text_preferences_changed_callback_ = std::move(text_preferences_changed_callback);
        footer_preferences_changed_callback_ = std::move(footer_preferences_changed_callback);
        window_preferences_changed_callback_ = std::move(window_preferences_changed_callback);
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
        set_text(FooterPageTitle(), L"FooterPageTitle.Text");
        set_text(FooterPageDescription(), L"FooterPageDescription.Text");
        set_text(FooterFieldsLabel(), L"FooterFieldsLabel.Text");
        set_text(FooterFieldsDescription(), L"FooterFieldsDescription.Text");
        set_content(FooterSizeCheckBox(), L"FooterSizeCheckBox.Content");
        set_content(FooterModifiedTimeCheckBox(), L"FooterModifiedTimeCheckBox.Content");
        set_content(FooterCreationTimeCheckBox(), L"FooterCreationTimeCheckBox.Content");
        set_content(FooterPermissionsCheckBox(), L"FooterPermissionsCheckBox.Content");
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
        set_text(WindowBehaviorSectionDescription(), L"WindowBehaviorSectionDescription.Text");
        set_text(DefaultWindowSizeLabel(), L"DefaultWindowSizeLabel.Text");
        set_text(DefaultWindowSizeDescription(), L"DefaultWindowSizeDescription.Text");
        set_text(RememberWindowSizeLabel(), L"RememberWindowSizeLabel.Text");
        set_text(RememberWindowSizeDescription(), L"RememberWindowSizeDescription.Text");
        set_text(AutoFitWindowSizeLabel(), L"AutoFitWindowSizeLabel.Text");
        set_text(AutoFitWindowSizeDescription(), L"AutoFitWindowSizeDescription.Text");
        set_text(ShowAfterAutoFitLabel(), L"ShowAfterAutoFitLabel.Text");
        set_text(ShowAfterAutoFitDescription(), L"ShowAfterAutoFitDescription.Text");
        set_text(AdaptiveMediaSizeSectionTitle(), L"AdaptiveMediaSizeSectionTitle.Text");
        set_text(AdaptiveMediaSizeSectionDescription(), L"AdaptiveMediaSizeSectionDescription.Text");
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
        set_text(PlainTextPreviewSectionDescription(), L"PlainTextPreviewSectionDescription.Text");
        set_text(OfficePreviewSectionTitle(), L"OfficePreviewSectionTitle.Text");
        set_text(OfficePreviewSectionDescription(), L"OfficePreviewSectionDescription.Text");
        set_text(OfficeCacheCapacityLabel(), L"OfficeCacheCapacityLabel.Text");
        set_text(OfficeCacheCapacityDescription(), L"OfficeCacheCapacityDescription.Text");
        set_text(OfficeCacheExpirationLabel(), L"OfficeCacheExpirationLabel.Text");
        set_text(OfficeCacheExpirationDescription(), L"OfficeCacheExpirationDescription.Text");
        set_text(OfficeCacheExpirationUnit(), L"OfficeCacheExpirationUnit.Text");
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
        set_text(PathCopyGroupDescription(), L"PathCopyGroupDescription.Text");
        set_text(QuoteCopiedPathLabel(), L"QuoteCopiedPathLabel.Text");
        set_text(QuoteCopiedPathDescription(), L"QuoteCopiedPathDescription.Text");
        set_text(UnixPathSeparatorsLabel(), L"UnixPathSeparatorsLabel.Text");
        set_text(UnixPathSeparatorsDescription(), L"UnixPathSeparatorsDescription.Text");
        set_text(MaintenancePageTitle(), L"MaintenancePageTitle.Text");
        set_text(MaintenancePageDescription(), L"MaintenancePageDescription.Text");
        set_text(InputCoreLabel(), L"InputCoreLabel.Text");
        set_text(MediaComponentsLabel(), L"MediaComponentsLabel.Text");
        set_text(WebViewAvailabilityLabel(), L"WebViewAvailabilityLabel.Text");
        set_text(OfficeAvailabilityLabel(), L"OfficeAvailabilityLabel.Text");
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
        AboutVersionText().Text(glance::app::localize_format(
            L"VersionFormat", { GLANCE_VERSION_WSTRING }));
        refresh_runtime_statuses();
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
        set_status_indicator(
            CoreStatusIcon(),
            CoreStatusText(),
            core_running,
            L"CoreRunning",
            L"CoreNotRunning");
        set_status_indicator(
            MediaComponentsStatusIcon(),
            MediaComponentsStatusText(),
            glance::app::media_probe_available(),
            L"MediaComponentsAvailable",
            L"MediaComponentsUnavailable");
        set_status_indicator(
            WebViewAvailabilityStatusIcon(),
            WebViewAvailabilityStatusText(),
            glance::app::webview_runtime_available(),
            L"WebViewAvailable",
            L"WebViewUnavailable");
        set_status_indicator(
            OfficeAvailabilityStatusIcon(),
            OfficeAvailabilityStatusText(),
            glance::app::office_com_available(),
            L"OfficeComAvailable",
            L"OfficeComUnavailable");
        set_status_indicator(
            AdministratorAccessStatusIcon(),
            AdministratorAccessStatusText(),
            core_running && named_mutex_exists(L"Local\\Glance.Core.Elevated"),
            L"AdministratorAccessAvailable",
            L"AdministratorAccessUnavailable");
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
        try
        {
            co_await resume_background();
            result = glance::app::check_for_updates(GLANCE_VERSION_WSTRING);
            co_await ui_thread;
        }
        catch (...)
        {
            co_return;
        }

        lifetime->update_check_in_progress_ = false;
        lifetime->CheckForUpdatesButton().IsEnabled(true);
        lifetime->UpdateCheckProgressRing().IsActive(false);
        lifetime->UpdateCheckProgressRing().Visibility(Visibility::Collapsed);
        if (lifetime->RootGrid().XamlRoot() == nullptr)
        {
            co_return;
        }

        try
        {
            Controls::ContentDialog dialog;
            dialog.XamlRoot(lifetime->RootGrid().XamlRoot());
            dialog.CloseButtonText(glance::app::localize(L"OK"));
            dialog.DefaultButton(Controls::ContentDialogButton::Close);

            switch (result.status)
            {
            case glance::app::UpdateCheckStatus::update_available:
                dialog.Title(box_value(glance::app::localize(L"UpdateAvailableTitle")));
                dialog.Content(box_value(glance::app::localize_format(
                    L"UpdateAvailableMessage", { result.latest_version })));
                dialog.PrimaryButtonText(glance::app::localize(L"UpdateAvailablePrimary"));
                dialog.CloseButtonText(glance::app::localize(L"Cancel"));
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

            const auto dialog_result = co_await dialog.ShowAsync();
            if (result.status == glance::app::UpdateCheckStatus::update_available &&
                dialog_result == Controls::ContentDialogResult::Primary)
            {
                static_cast<void>(co_await Windows::System::Launcher::LaunchUriAsync(
                    Windows::Foundation::Uri(L"https://github.com/ElluIFX/Glance/releases/latest")));
            }
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

    void SettingsWindow::OfficeCacheCapacitySlider_ValueChanged(
        IInspectable const&,
        Controls::Primitives::RangeBaseValueChangedEventArgs const& args)
    {
        const auto capacity = static_cast<std::uint32_t>(
            std::clamp(std::lround(args.NewValue()), 0L, 16L));
        OfficeCacheCapacityValueText().Text(std::to_wstring(capacity));
        if (initializing_)
        {
            return;
        }

        office_preview_preferences_.cache_capacity = capacity;
        glance::app::save_office_preview_preferences(office_preview_preferences_);
        glance::app::configure_office_preview_cache(office_preview_preferences_);
    }

    void SettingsWindow::OfficeCacheExpirationNumberBox_ValueChanged(
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
            OfficeCacheExpirationNumberBox().Value(
                office_preview_preferences_.cache_expiration_minutes);
            initializing_ = false;
            return;
        }

        office_preview_preferences_.cache_expiration_minutes = static_cast<std::uint32_t>(
            std::clamp(std::lround(args.NewValue()), 1L, 60L));
        initializing_ = true;
        OfficeCacheExpirationNumberBox().Value(
            office_preview_preferences_.cache_expiration_minutes);
        initializing_ = false;
        glance::app::save_office_preview_preferences(office_preview_preferences_);
        glance::app::configure_office_preview_cache(office_preview_preferences_);
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
        case FooterField::permissions:
            return { FooterPermissionsRow(), FooterPermissionsCheckBox(), FooterPermissionsMoveUpButton(), FooterPermissionsMoveDownButton() };
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
    }

    fire_and_forget SettingsWindow::ExitButton_Click(IInspectable const&, RoutedEventArgs const&)
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
        Close();
    }
}

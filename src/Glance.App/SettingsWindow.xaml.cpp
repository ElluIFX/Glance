#include "pch.h"
#include "SettingsWindow.xaml.h"
#include "appearance_preferences.h"
#include "localization.h"
#include "path_copy_preferences.h"
#include "resource.h"
#include "startup_registration.h"
#include "text_preferences.h"
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

namespace
{
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
}

namespace winrt::Glance::App::implementation
{
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
        appearance_preferences_ = glance::app::load_appearance_preferences();
        LanguageComboBox().SelectedIndex(appearance_preferences_.language == L"zh-CN" ? 1 : 0);
        ThemeComboBox().SelectedIndex(static_cast<int>(appearance_preferences_.theme));
        AccentComboBox().SelectedIndex(static_cast<int>(appearance_preferences_.accent));
        LaunchAtSignInToggle().IsOn(launch_at_sign_in_enabled());
        DiagnosticsToggle().IsOn(glance::contracts::diagnostics_enabled());
        AutoFitWindowSizeToggle().IsOn(glance::app::auto_fit_window_size_enabled());
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
        LineNumbersToggle().IsOn(text_preferences_.line_numbers);
        WordWrapToggle().IsOn(text_preferences_.word_wrap);
        path_copy_preferences_ = glance::app::load_path_copy_preferences();
        QuoteCopiedPathToggle().IsOn(path_copy_preferences_.quote_path);
        UnixPathSeparatorsToggle().IsOn(path_copy_preferences_.use_unix_separators);
        initializing_ = false;
        refresh_core_status();
        refresh_diagnostic_bundle_status();
    }

    void SettingsWindow::InitializeSession(
        ExitCallback exit_callback,
        AppearanceChangedCallback appearance_changed_callback,
        TextPreferencesChangedCallback text_preferences_changed_callback)
    {
        exit_callback_ = std::move(exit_callback);
        appearance_changed_callback_ = std::move(appearance_changed_callback);
        text_preferences_changed_callback_ = std::move(text_preferences_changed_callback);
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

    void SettingsWindow::ApplyLocalizedResources()
    {
        const auto set_text = [](const auto& control, wchar_t const* key) {
            control.Text(glance::app::localize(key));
        };
        const auto set_content = [](const auto& control, wchar_t const* key) {
            control.Content(box_value(glance::app::localize(key)));
        };

        Title(glance::app::localize(L"SettingsTitle"));
        set_text(SettingsTitleText(), L"SettingsTitleText.Text");
        Controls::ToolTipService::SetToolTip(
            CloseSettingsButton(),
            box_value(glance::app::localize(L"CloseSettingsButton.ToolTipService.ToolTip")));
        set_content(GeneralNavigationItem(), L"GeneralNavigationItem.Content");
        set_content(TextPreviewNavigationItem(), L"TextPreviewNavigationItem.Content");
        set_content(PathCopyNavigationItem(), L"PathCopyNavigationItem.Content");
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
        set_text(DiagnosticsTitle(), L"DiagnosticsTitle.Text");
        set_text(DiagnosticsDescription(), L"DiagnosticsDescription.Text");
        set_text(AutoFitWindowSizeLabel(), L"AutoFitWindowSizeLabel.Text");
        set_text(AutoFitWindowSizeDescription(), L"AutoFitWindowSizeDescription.Text");
        set_text(WindowSizesLabel(), L"WindowSizesLabel.Text");
        set_text(WindowSizeResetStatusText(), L"WindowSizeResetStatusText.Text");
        set_content(ResetWindowSizesButton(), L"ResetWindowSizesButton.Content");
        set_text(TextPreviewPageTitle(), L"TextPreviewPageTitle.Text");
        set_text(TextPreviewPageDescription(), L"TextPreviewPageDescription.Text");
        set_text(FontFamilyLabel(), L"FontFamilyLabel.Text");
        set_text(FontSizeLabel(), L"FontSizeLabel.Text");
        set_text(SyntaxHighlightingLabel(), L"SyntaxHighlightingLabel.Text");
        set_text(LineNumbersLabel(), L"LineNumbersLabel.Text");
        set_text(WordWrapLabel(), L"WordWrapLabel.Text");
        set_text(PathCopyPageTitle(), L"PathCopyPageTitle.Text");
        set_text(PathCopyPageDescription(), L"PathCopyPageDescription.Text");
        set_text(QuoteCopiedPathLabel(), L"QuoteCopiedPathLabel.Text");
        set_text(QuoteCopiedPathDescription(), L"QuoteCopiedPathDescription.Text");
        set_text(UnixPathSeparatorsLabel(), L"UnixPathSeparatorsLabel.Text");
        set_text(UnixPathSeparatorsDescription(), L"UnixPathSeparatorsDescription.Text");
        set_text(MaintenancePageTitle(), L"MaintenancePageTitle.Text");
        set_text(MaintenancePageDescription(), L"MaintenancePageDescription.Text");
        set_text(InputCoreLabel(), L"InputCoreLabel.Text");
        Controls::ToolTipService::SetToolTip(
            RefreshCoreButton(),
            box_value(glance::app::localize(L"RefreshCoreButton.ToolTipService.ToolTip")));
        set_text(DiagnosticBundleLabel(), L"DiagnosticBundleLabel.Text");
        set_content(ExportDiagnosticBundleButton(), L"ExportDiagnosticBundleButton.Content");
        refresh_diagnostic_bundle_status();
        set_text(AboutPageTitle(), L"AboutPageTitle.Text");
        set_text(AboutPageDescription(), L"AboutPageDescription.Text");
        set_text(AboutAuthorLabel(), L"AboutAuthorLabel.Text");
        set_text(AboutLicenseText(), L"AboutLicenseText.Text");
        set_content(AboutProjectLink(), L"AboutProjectLink.Content");
        AboutVersionText().Text(glance::app::localize_format(
            L"VersionFormat", { GLANCE_VERSION_WSTRING }));
        refresh_core_status();
    }

    bool SettingsWindow::launch_at_sign_in_enabled() const
    {
        return glance::app::launch_at_sign_in_enabled();
    }

    void SettingsWindow::set_launch_at_sign_in(bool enabled)
    {
        if (glance::app::set_launch_at_sign_in(enabled))
        {
            return;
        }
        initializing_ = true;
        LaunchAtSignInToggle().IsOn(launch_at_sign_in_enabled());
        initializing_ = false;
    }

    void SettingsWindow::refresh_core_status()
    {
        HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\Glance.Core");
        CoreStatusText().Text(glance::app::localize(
            mutex != nullptr ? L"CoreRunning" : L"CoreNotRunning"));
        if (mutex != nullptr)
        {
            CloseHandle(mutex);
        }
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

    void SettingsWindow::AutoFitWindowSizeToggle_Toggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (!initializing_)
        {
            glance::app::set_auto_fit_window_size_enabled(AutoFitWindowSizeToggle().IsOn());
        }
    }

    void SettingsWindow::RefreshCoreStatusButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        refresh_core_status();
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

    void SettingsWindow::ResetWindowSizesButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        WindowSizeResetStatusText().Text(
            glance::app::clear_window_sizes()
                ? glance::app::localize(L"SizesReset")
                : glance::app::localize(L"SizesResetFailed"));
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
        if (!initializing_ && std::isfinite(args.NewValue()))
        {
            text_preferences_.font_size = args.NewValue();
            save_text_preferences();
        }
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
        TextPreviewSettingsPanel().Visibility(tag == L"text" ? Visibility::Visible : Visibility::Collapsed);
        PathCopySettingsPanel().Visibility(tag == L"path" ? Visibility::Visible : Visibility::Collapsed);
        MaintenanceSettingsPanel().Visibility(tag == L"maintenance" ? Visibility::Visible : Visibility::Collapsed);
        AboutSettingsPanel().Visibility(tag == L"about" ? Visibility::Visible : Visibility::Collapsed);
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

#include "pch.h"
#include "SettingsWindow.xaml.h"
#include "appearance_preferences.h"
#include "localization.h"
#include "path_copy_preferences.h"
#include "resource.h"
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
#include <ranges>
#include <string_view>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
namespace Controls = Microsoft::UI::Xaml::Controls;

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
        Title(glance::app::localize(L"SettingsTitle"));
        SettingsTitleText().Text(glance::app::localize(L"SettingsTitle"));
        AboutVersionText().Text(L"Version " GLANCE_VERSION_WSTRING);
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
        LanguageComboBox().SelectedIndex(0);
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
    }

    void SettingsWindow::InitializeSession(
        ExitCallback exit_callback,
        AppearanceChangedCallback appearance_changed_callback)
    {
        exit_callback_ = std::move(exit_callback);
        appearance_changed_callback_ = std::move(appearance_changed_callback);
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
        CoreStatusText().Text(glance::app::localize(
            mutex != nullptr ? L"CoreRunning" : L"CoreNotRunning"));
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
        appearance_preferences_.language = L"en-US";
        appearance_preferences_.theme = static_cast<glance::app::ThemePreference>(
            std::clamp(ThemeComboBox().SelectedIndex(), 0, 2));
        appearance_preferences_.accent = static_cast<glance::app::AccentPreference>(
            std::clamp(AccentComboBox().SelectedIndex(), 0, 7));
        glance::app::save_appearance_preferences(appearance_preferences_);
        glance::app::apply_accent_resources(appearance_preferences_);
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
            lifetime->exit_callback_();
        }
    }

    void SettingsWindow::CloseSettingsButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        Close();
    }
}

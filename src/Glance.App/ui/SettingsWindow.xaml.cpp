#include "pch.h"
#include "SettingsWindow.xaml.h"
#include "App.xaml.h"
#include "core_task.h"
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
#include "../../version.h"
#if __has_include("SettingsWindow.g.cpp")
#include "SettingsWindow.g.cpp"
#endif

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

namespace
{

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
        register_general_settings();
        register_window_settings();
        register_footer_settings();
        register_text_settings();
        register_media_settings();
        register_components_settings();
        register_maintenance_settings();
        register_component_settings();
        settings_registry_.initialize(SettingsNavigation(), SettingsPageHost());
        settings_registry_.show_page(L"general");
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
                if (self->first_frame_rendered_token_.value != 0)
                {
                    Media::CompositionTarget::Rendered(self->first_frame_rendered_token_);
                    self->first_frame_rendered_token_ = {};
                }
                self->acrylic_backdrop_.reset();
                if (self->size_reset_timer_) self->size_reset_timer_.Stop();
                if (self->position_reset_timer_) self->position_reset_timer_.Stop();
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

        ReloadPreferences();
        initializing_ = false;
        refresh_diagnostic_bundle_status();
        Activated([this](IInspectable const&, WindowActivatedEventArgs const& args) {
            if (acrylic_backdrop_ != nullptr)
            {
                acrylic_backdrop_->set_input_active(
                    args.WindowActivationState() != WindowActivationState::Deactivated);
            }
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
        ComponentSettingChangedCallback component_setting_changed_callback,
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
        component_setting_changed_callback_ =
            std::move(component_setting_changed_callback);
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
        const auto preferences = glance::app::load_appearance_preferences();
        RootGrid().RequestedTheme(glance::app::element_theme(preferences.theme));
        if (preferences.acrylic_enabled && glance::app::acrylic_material_supported())
        {
            if (acrylic_backdrop_ == nullptr)
            {
                acrylic_backdrop_ = glance::app::WindowAcrylicBackdrop::create(
                    *this,
                    RootGrid(),
                    true,
                    preferences.acrylic_opacity_percent);
            }
            else
            {
                acrylic_backdrop_->set_opacity(preferences.acrylic_opacity_percent);
            }
        }
        else
        {
            acrylic_backdrop_.reset();
        }
        apply_background_surface(acrylic_backdrop_ != nullptr);
    }

    void SettingsWindow::apply_background_surface(bool acrylic_enabled)
    {
        if (acrylic_enabled)
        {
            RootGrid().Background(Media::SolidColorBrush(
                Windows::UI::Color{ 0, 0, 0, 0 }));
        }
        else
        {
            RootGrid().ClearValue(Controls::Panel::BackgroundProperty());
        }
    }

    void SettingsWindow::ShowAndActivate()
    {
        request_source_statuses();
        HWND window{};
        if (FAILED(this->try_as<::IWindowNative>()->get_WindowHandle(&window)) || window == nullptr)
        {
            Activate();
            return;
        }

        if (IsWindowVisible(window))
        {
            ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOW);
            Activate();
            SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            SetForegroundWindow(window);
            SetActiveWindow(window);
            return;
        }

        BOOL cloaked = TRUE;
        const bool cloak_applied = SUCCEEDED(
            DwmSetWindowAttribute(window, DWMWA_CLOAK, &cloaked, sizeof(cloaked)));
        if (cloak_applied)
        {
            const auto weak = get_weak();
            first_frame_rendered_token_ = Media::CompositionTarget::Rendered(
                [weak, window](IInspectable const&, Media::RenderedEventArgs const&) {
                    const auto self = weak.get();
                    if (self == nullptr)
                    {
                        return;
                    }
                    Media::CompositionTarget::Rendered(self->first_frame_rendered_token_);
                    self->first_frame_rendered_token_ = {};
                    UpdateWindow(window);
                    static_cast<void>(DwmFlush());
                    BOOL reveal = FALSE;
                    static_cast<void>(
                        DwmSetWindowAttribute(window, DWMWA_CLOAK, &reveal, sizeof(reveal)));
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
                });
        }
        ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOW);
        UpdateWindow(window);
        if (!cloak_applied)
        {
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
    }

    void SettingsWindow::SettingsNavigation_SelectionChanged(
        Controls::NavigationView const&,
        Controls::NavigationViewSelectionChangedEventArgs const& args)
    {
        const auto selected = args.SelectedItemContainer();
        const hstring tag = selected == nullptr
            ? L"general"
            : unbox_value_or<hstring>(selected.Tag(), L"general");
        const bool first_visit = !settings_registry_.page_created(tag.c_str());
        const bool was_initializing = initializing_;
        initializing_ = true;
        settings_registry_.show_page(tag.c_str());
        if (first_visit) ReloadPreferences(tag.c_str());
        initializing_ = was_initializing;
        AboutSettingsPanel().Visibility(tag == L"about" ? Visibility::Visible : Visibility::Collapsed);
        if (tag == L"about")
            Hosting::ElementCompositionPreview::GetElementVisual(AboutSettingsPanel()).Opacity(1);
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

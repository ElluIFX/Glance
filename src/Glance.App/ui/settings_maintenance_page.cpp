#include "pch.h"
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include "SettingsWindow.xaml.h"
#include "App.xaml.h"
#include "access_setup.h"
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
    constexpr wchar_t latest_release_url[] = L"https://github.com/ElluIFX/Glance/releases/latest";

    std::wstring safe_release_url(std::wstring_view value)
    {
        constexpr std::wstring_view release_prefix = L"https://github.com/ElluIFX/Glance/releases/";
        return value.starts_with(release_prefix) ? std::wstring(value) : std::wstring(latest_release_url);
    }

    std::wstring format_megabytes(std::uint64_t bytes)
    {
        wchar_t text[32]{};
        swprintf_s(text, L"%.1f", static_cast<double>(bytes) / (1024.0 * 1024.0));
        return text;
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
        swprintf_s(value, L"%04u%02u%02u-%02u%02u%02u", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
                   time.wSecond);
        return value;
    }

    std::optional<std::filesystem::path> select_output_directory(HWND owner, std::wstring_view title)
    {
        com_ptr<IFileOpenDialog> dialog;
        check_hresult(
            CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())));
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

    std::pair<bool, std::wstring> create_diagnostic_bundle(const std::filesystem::path &output_directory)
    {
        const std::filesystem::path diagnostics_root(glance::contracts::diagnostics_root_path());
        if (diagnostics_root.empty())
        {
            return {false, {}};
        }

        wchar_t system_directory[MAX_PATH]{};
        const UINT system_directory_length = GetSystemDirectoryW(system_directory, ARRAYSIZE(system_directory));
        if (system_directory_length == 0 || system_directory_length >= ARRAYSIZE(system_directory))
        {
            return {false, {}};
        }
        const std::filesystem::path tar_path = std::filesystem::path(system_directory) / L"tar.exe";
        if (!std::filesystem::exists(tar_path))
        {
            return {false, {}};
        }

        const auto output_path = output_directory / (L"Glance-Diagnostics-" + bundle_timestamp() + L"-" +
                                                     std::to_wstring(GetCurrentProcessId()) + L".zip");
        std::wstring command_line = quote_argument(tar_path.wstring()) + L" -a -c -f " +
                                    quote_argument(output_path.wstring()) + L" -C " +
                                    quote_argument(diagnostics_root.wstring()) + L" Logs Dumps";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(tar_path.c_str(), command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                            nullptr, &startup, &process))
        {
            return {false, {}};
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
            return {false, {}};
        }
        return {true, output_path.wstring()};
    }

    bool named_mutex_exists(const wchar_t *name) noexcept
    {
        const HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, name);
        if (mutex == nullptr)
        {
            return false;
        }
        CloseHandle(mutex);
        return true;
    }

    void set_status_indicator(const Controls::FontIcon &icon, const Controls::TextBlock &text, bool available,
                              const wchar_t *available_resource, const wchar_t *unavailable_resource)
    {
        const auto status = glance::app::localize(available ? available_resource : unavailable_resource);
        text.Text(status);
        icon.Glyph(available ? L"\xE8FB" : L"\xE711");
        icon.Foreground(Media::SolidColorBrush(available ? Windows::UI::Color{255, 16, 124, 16}
                                                         : Windows::UI::Color{255, 196, 43, 28}));
        Controls::ToolTipService::SetToolTip(icon, box_value(status));
    }

} // namespace

namespace winrt::Glance::App::implementation
{
    void SettingsWindow::register_maintenance_settings()
    {
        settings_registry_.register_page({L"maintenance", L"", L"MaintenanceNavigationItem.Content",
                                          L"MaintenancePageDescription.Text",
                                          glance::app::SettingsNavigationPosition::bottom});
        settings_registry_.register_section(
            {L"RuntimeStatusGroupTitle", L"maintenance", L"RuntimeStatusGroupTitle.Text", {}});
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"InputCoreLabel";
            definition.parent = L"RuntimeStatusGroupTitle";
            definition.name_key = L"InputCoreLabel.Text";
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<FontIcon xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" x:Name="CoreStatusIcon" FontSize="18" HorizontalAlignment="Right" VerticalAlignment="Center" /> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"CoreStatusIcon", control);
                return control;
            };
            settings_registry_.bind_factory(L"CoreStatusIcon", [this] {
                settings_registry_.item(L"InputCoreLabel");
                return settings_registry_.control(L"CoreStatusIcon");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"AdministratorAccessLabel";
            definition.parent = L"RuntimeStatusGroupTitle";
            definition.name_key = L"AdministratorAccessLabel.Text";
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<StackPanel xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" Orientation="Horizontal" Spacing="12" VerticalAlignment="Center">
<Button x:Name="RepairCoreAccessButton" Content="Enable / repair" Visibility="Collapsed" IsTabStop="False" AllowFocusOnInteraction="False" />
<FontIcon x:Name="AdministratorAccessStatusIcon" FontSize="18" VerticalAlignment="Center" />
</StackPanel> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"RepairCoreAccessButton",
                                        control.FindName(L"RepairCoreAccessButton").as<FrameworkElement>());
                settings_registry_.bind(L"AdministratorAccessStatusIcon",
                                        control.FindName(L"AdministratorAccessStatusIcon").as<FrameworkElement>());
                const auto weak = get_weak();
                RepairCoreAccessButton().Click([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->RepairCoreAccessButton_Click(sender, args);
                });
                settings_registry_.bind_text(L"RepairCoreAccessButton", L"RepairCoreAccessButton.Content", true);
                RepairCoreAccessButton().Content(box_value(glance::app::localize(L"RepairCoreAccessButton.Content")));
                return control;
            };
            settings_registry_.bind_factory(L"RepairCoreAccessButton", [this] {
                settings_registry_.item(L"AdministratorAccessLabel");
                return settings_registry_.control(L"RepairCoreAccessButton");
            });
            settings_registry_.bind_factory(L"AdministratorAccessStatusIcon", [this] {
                settings_registry_.item(L"AdministratorAccessLabel");
                return settings_registry_.control(L"AdministratorAccessStatusIcon");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"WebViewAvailabilityLabel";
            definition.parent = L"RuntimeStatusGroupTitle";
            definition.name_key = L"WebViewAvailabilityLabel.Text";
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<StackPanel xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" VerticalAlignment="Center" Orientation="Horizontal" Spacing="12">
<HyperlinkButton x:Name="WebViewDownloadLink" Padding="0" Content="Download WebView2" NavigateUri="https://developer.microsoft.com/en-us/microsoft-edge/webview2/" Visibility="Collapsed" IsTabStop="False" AllowFocusOnInteraction="False" />
<FontIcon x:Name="WebViewAvailabilityStatusIcon" FontSize="18" VerticalAlignment="Center" />
</StackPanel> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"WebViewDownloadLink",
                                        control.FindName(L"WebViewDownloadLink").as<FrameworkElement>());
                settings_registry_.bind(L"WebViewAvailabilityStatusIcon",
                                        control.FindName(L"WebViewAvailabilityStatusIcon").as<FrameworkElement>());
                settings_registry_.bind_text(L"WebViewDownloadLink", L"WebViewDownloadLink.Content", true);
                WebViewDownloadLink().Content(box_value(glance::app::localize(L"WebViewDownloadLink.Content")));
                return control;
            };
            settings_registry_.bind_factory(L"WebViewDownloadLink", [this] {
                settings_registry_.item(L"WebViewAvailabilityLabel");
                return settings_registry_.control(L"WebViewDownloadLink");
            });
            settings_registry_.bind_factory(L"WebViewAvailabilityStatusIcon", [this] {
                settings_registry_.item(L"WebViewAvailabilityLabel");
                return settings_registry_.control(L"WebViewAvailabilityStatusIcon");
            });
            settings_registry_.register_item(std::move(definition));
        }
        settings_registry_.register_section(
            {L"MaintenanceActionsGroupTitle", L"maintenance", L"MaintenanceActionsGroupTitle.Text", {}});
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"DiagnosticsTitle";
            definition.parent = L"MaintenanceActionsGroupTitle";
            definition.name_key = L"DiagnosticsTitle.Text";
            definition.description = [this] {
                return glance::app::SettingsText{DiagnosticsToggle().IsOn() ? L"DiagnosticsEnabledDescription.Text"
                                                                            : L"DiagnosticsDisabledDescription.Text",
                                                 {}};
            };
            definition.create_control = [this] {
                auto control = glance::app::make_settings_toggle();
                settings_registry_.bind(L"DiagnosticsToggle", control);
                const auto weak = get_weak();
                DiagnosticsToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->DiagnosticsToggle_Toggled(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"DiagnosticsToggle", [this] {
                settings_registry_.item(L"DiagnosticsTitle");
                return settings_registry_.control(L"DiagnosticsToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"DiagnosticBundleLabel";
            definition.parent = L"MaintenanceActionsGroupTitle";
            definition.name_key = L"DiagnosticBundleLabel.Text";
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" x:Name="ExportDiagnosticBundleButton" MinWidth="88" HorizontalAlignment="Right" VerticalAlignment="Center" Content="Export" IsTabStop="False" AllowFocusOnInteraction="False" /> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"ExportDiagnosticBundleButton", control);
                const auto weak = get_weak();
                ExportDiagnosticBundleButton().Click([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->ExportDiagnosticBundleButton_Click(sender, args);
                });
                settings_registry_.bind_text(L"ExportDiagnosticBundleButton", L"ExportDiagnosticBundleButton.Content",
                                             true);
                ExportDiagnosticBundleButton().Content(
                    box_value(glance::app::localize(L"ExportDiagnosticBundleButton.Content")));
                return control;
            };
            settings_registry_.bind_factory(L"ExportDiagnosticBundleButton", [this] {
                settings_registry_.item(L"DiagnosticBundleLabel");
                return settings_registry_.control(L"ExportDiagnosticBundleButton");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"ResetAllSettingsLabel";
            definition.parent = L"MaintenanceActionsGroupTitle";
            definition.name_key = L"ResetAllSettingsLabel.Text";
            definition.description_key = L"ResetAllSettingsDescription.Text";
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" x:Name="ResetAllSettingsButton" MinWidth="88" HorizontalAlignment="Right" VerticalAlignment="Center" Content="Reset" IsTabStop="False" AllowFocusOnInteraction="False" /> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"ResetAllSettingsButton", control);
                const auto weak = get_weak();
                ResetAllSettingsButton().Click([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->ResetAllSettingsButton_Click(sender, args);
                });
                settings_registry_.bind_text(L"ResetAllSettingsButton", L"ResetAllSettingsButton.Content", true);
                ResetAllSettingsButton().Content(box_value(glance::app::localize(L"ResetAllSettingsButton.Content")));
                return control;
            };
            settings_registry_.bind_factory(L"ResetAllSettingsButton", [this] {
                settings_registry_.item(L"ResetAllSettingsLabel");
                return settings_registry_.control(L"ResetAllSettingsButton");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"CliPathLabel";
            definition.parent = L"MaintenanceActionsGroupTitle";
            definition.name_key = L"CliPathLabel.Text";
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" x:Name="CopyCliPathButton" MinWidth="88" HorizontalAlignment="Right" VerticalAlignment="Center" Content="Copy path" IsTabStop="False" AllowFocusOnInteraction="False" /> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"CopyCliPathButton", control);
                const auto weak = get_weak();
                CopyCliPathButton().Click([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->CopyCliPathButton_Click(sender, args);
                });
                settings_registry_.bind_text(L"CopyCliPathButton", L"CopyCliPathButton.Content", true);
                CopyCliPathButton().Content(box_value(glance::app::localize(L"CopyCliPathButton.Content")));
                return control;
            };
            settings_registry_.bind_factory(L"CopyCliPathButton", [this] {
                settings_registry_.item(L"CliPathLabel");
                return settings_registry_.control(L"CopyCliPathButton");
            });
            settings_registry_.register_item(std::move(definition));
        }
    }

    void SettingsWindow::refresh_runtime_statuses()
    {
        if (!settings_registry_.page_created(L"maintenance"))
            return;
        const bool core_running = named_mutex_exists(L"Local\\Glance.Core");
        const bool webview_available = glance::app::webview_runtime_available();
        set_status_indicator(CoreStatusIcon(), CoreStatusText(), core_running, L"CoreRunning", L"CoreNotRunning");
        set_status_indicator(WebViewAvailabilityStatusIcon(), WebViewAvailabilityStatusText(), webview_available,
                             L"WebViewAvailable", L"WebViewUnavailable");
        WebViewDownloadLink().Visibility(webview_available ? Visibility::Collapsed : Visibility::Visible);
        const bool administrator_access = Application::Current().as<implementation::App>()->HasAdministratorAccess();
        set_status_indicator(AdministratorAccessStatusIcon(), AdministratorAccessStatusText(), administrator_access,
                             L"AdministratorAccessAvailable", L"AdministratorAccessUnavailable");
        RepairCoreAccessButton().Visibility(administrator_access ? Visibility::Collapsed : Visibility::Visible);
    }

    fire_and_forget SettingsWindow::RepairCoreAccessButton_Click(IInspectable const &, RoutedEventArgs const &)
    {
        const auto lifetime = get_strong();
        const apartment_context ui;
        RepairCoreAccessButton().IsEnabled(false);
        auto result = glance::app::CoreAccessResult::failed;
        try
        {
            HWND owner{};
            check_hresult(this->try_as<::IWindowNative>()->get_WindowHandle(&owner));
            co_await resume_background();
            result = glance::app::repair_access_service(owner);
            co_await ui;
            if (result == glance::app::CoreAccessResult::success)
            {
                const auto app = Application::Current().as<implementation::App>();
                const HRESULT status = co_await app->RestartCoreAfterAccessRepair();
                if (FAILED(status))
                    result = status == HRESULT_FROM_WIN32(ERROR_CANCELLED) ? glance::app::CoreAccessResult::cancelled
                                                                           : glance::app::CoreAccessResult::failed;
            }
        }
        catch (...)
        {
        }
        co_await ui;
        RepairCoreAccessButton().IsEnabled(true);
        refresh_runtime_statuses();
        if (result == glance::app::CoreAccessResult::success || result == glance::app::CoreAccessResult::cancelled)
            co_return;
        try
        {
            Controls::ContentDialog dialog;
            dialog.XamlRoot(RootGrid().XamlRoot());
            dialog.Title(box_value(glance::app::localize(L"AdministratorAccessLabel.Text")));
            const auto key = result == glance::app::CoreAccessResult::unsafe_location ? L"CoreAccessUnsafeLocation"
                             : result == glance::app::CoreAccessResult::administrator_required
                                 ? L"CoreAccessAdministratorRequired"
                                 : L"CoreAccessRepairFailed";
            dialog.Content(box_value(glance::app::localize(key)));
            dialog.CloseButtonText(glance::app::localize(L"OK"));
            co_await dialog.ShowAsync();
        }
        catch (...)
        {
        }
    }

    void SettingsWindow::refresh_diagnostic_bundle_status()
    {
        if (!settings_registry_.page_created(L"maintenance"))
            return;
        switch (diagnostic_bundle_state_)
        {
        case DiagnosticBundleState::packaging:
            DiagnosticBundleStatusText().Text(glance::app::localize(L"DiagnosticBundlePackaging"));
            break;
        case DiagnosticBundleState::succeeded:
            DiagnosticBundleStatusText().Text(
                glance::app::localize_format(L"DiagnosticBundleCreated", {diagnostic_bundle_path_}));
            break;
        case DiagnosticBundleState::failed:
            DiagnosticBundleStatusText().Text(glance::app::localize(L"DiagnosticBundleFailed"));
            break;
        default:
            DiagnosticBundleStatusText().Text(glance::app::localize(L"DiagnosticBundleDescription"));
            break;
        }
    }

    void SettingsWindow::DiagnosticsToggle_Toggled(IInspectable const &, RoutedEventArgs const &)
    {
        refresh_toggle_descriptions();
        if (!initializing_)
        {
            glance::contracts::set_diagnostics_enabled(DiagnosticsToggle().IsOn());
        }
    }

    void SettingsWindow::CopyCliPathButton_Click(IInspectable const &, RoutedEventArgs const &)
    {
        try
        {
            std::wstring executable(32768, L'\0');
            const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
            if (!length || length >= executable.size())
                throw_last_error();
            executable.resize(length);
            const auto path = std::filesystem::path(executable).parent_path() / L"Glance.CLI.exe";
            Windows::ApplicationModel::DataTransfer::DataPackage package;
            package.SetText(L"\"" + path.wstring() + L"\"");
            Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
            Windows::ApplicationModel::DataTransfer::Clipboard::Flush();
            CopyCliPathButton().Content(box_value(glance::app::localize(L"CliPathCopied")));
        }
        catch (const hresult_error &error)
        {
            glance::contracts::log_event(L"Copy CLI path failed: " + std::wstring(error.message()));
            CopyCliPathButton().Content(box_value(glance::app::localize(L"CliPathCopyFailed")));
        }
    }

    fire_and_forget SettingsWindow::ExportDiagnosticBundleButton_Click(IInspectable const &, RoutedEventArgs const &)
    {
        const auto lifetime = get_strong();
        std::optional<std::filesystem::path> output_directory;
        try
        {
            HWND window{};
            check_hresult(this->try_as<::IWindowNative>()->get_WindowHandle(&window));
            output_directory = select_output_directory(window, glance::app::localize(L"DiagnosticBundlePickerTitle"));
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
            lifetime->diagnostic_bundle_state_ =
                succeeded ? DiagnosticBundleState::succeeded : DiagnosticBundleState::failed;
            lifetime->diagnostic_bundle_path_ = output_path;
            lifetime->ExportDiagnosticBundleButton().IsEnabled(true);
            lifetime->refresh_diagnostic_bundle_status();
        }));
    }

    fire_and_forget SettingsWindow::ResetAllSettingsButton_Click(IInspectable const &, RoutedEventArgs const &)
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
        const bool registry_cleared = registry_result == ERROR_SUCCESS || registry_result == ERROR_FILE_NOT_FOUND ||
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

    fire_and_forget SettingsWindow::CheckForUpdatesButton_Click(IInspectable const &, RoutedEventArgs const &)
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

        const auto dialog_result = co_await ShowUpdateResultDialog(lifetime->RootGrid().XamlRoot(), result, false);
        if (dialog_result == static_cast<std::int32_t>(UpdatePromptResult::download))
        {
            lifetime->ShowUpdateDownload(std::move(result.installer));
        }
    }

    fire_and_forget SettingsWindow::download_and_install_update(glance::app::UpdateInstallerAsset asset)
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
        download_result =
            network_download_callback_
                ? network_download_callback_(
                      glance::contracts::NetworkDownloadRequest{asset.download_url, asset.file_name, asset.sha256,
                                                                asset.size},
                      *cancellation,
                      [dispatcher, weak, cancellation](std::uint64_t downloaded, std::uint64_t total) {
                          static_cast<void>(dispatcher.TryEnqueue([weak, cancellation, downloaded, total] {
                              if (const auto self = weak.get(); self != nullptr &&
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
            const wchar_t *message_key = L"UpdateDownloadNetworkFailedMessage";
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
        if (lifetime->RootGrid().XamlRoot() == nullptr || cancellation->load(std::memory_order_acquire))
        {
            co_return;
        }

        lifetime->show_update_installing_card();
        glance::app::UpdateLaunchStatus launch_status{};
        co_await resume_background();
        launch_status = glance::app::launch_update_installer(download_result.path);
        co_await ui_thread;
        if (launch_status == glance::app::UpdateLaunchStatus::launched || lifetime->RootGrid().XamlRoot() == nullptr)
        {
            co_return;
        }

        lifetime->hide_update_card();
        try
        {
            Controls::ContentDialog dialog;
            dialog.XamlRoot(lifetime->RootGrid().XamlRoot());
            dialog.Title(box_value(glance::app::localize(launch_status == glance::app::UpdateLaunchStatus::cancelled
                                                             ? L"UpdateLaunchCancelledTitle"
                                                             : L"UpdateLaunchFailedTitle")));
            dialog.Content(box_value(glance::app::localize(launch_status == glance::app::UpdateLaunchStatus::cancelled
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
                static_cast<void>(
                    co_await Windows::System::Launcher::LaunchUriAsync(Windows::Foundation::Uri(latest_release_url)));
            }
        }
        catch (...)
        {
        }
    }

    void SettingsWindow::show_update_download_card(std::wstring_view version)
    {
        show_download_card(glance::app::localize(L"UpdateDownloadTitle"),
                           glance::app::localize_format(L"UpdateDownloadMessage", {version}));
    }

    void SettingsWindow::show_download_card(std::wstring_view title, std::wstring_view message)
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
            L"UpdateDownloadBytesFormat", {format_megabytes(0), format_megabytes(update_total_bytes_)}));
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
            L"UpdateDownloadBytesFormat", {format_megabytes(downloaded), format_megabytes(total)}));
        const double target =
            std::max(update_target_progress_, static_cast<double>(downloaded) * 100.0 / static_cast<double>(total));
        if (!update_animations_enabled_)
        {
            update_displayed_progress_ = target;
            update_target_progress_ = target;
            UpdateProgressRing().Value(target);
            UpdateProgressPercentText().Text(std::to_wstring(static_cast<int>(std::floor(target))) + L"%");
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
        update_displayed_progress_ =
            update_start_progress_ + (update_target_progress_ - update_start_progress_) * eased;
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
        show_preparing_card(glance::app::localize(L"UpdateInstallingTitle"),
                            glance::app::localize(L"UpdateInstallingMessage"));
    }

    void SettingsWindow::show_preparing_card(std::wstring_view title, std::wstring_view message)
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

    void SettingsWindow::CancelUpdateButton_Click(IInspectable const &, RoutedEventArgs const &)
    {
        cancel_update_download();
    }
    void SettingsWindow::ShowUpdateDownload(glance::app::UpdateInstallerAsset asset)
    {
        SettingsNavigation().SelectedItem(AboutNavigationItem());
        download_and_install_update(std::move(asset));
    }

    Windows::Foundation::IAsyncOperation<std::int32_t> SettingsWindow::ShowUpdateResultDialog(
        XamlRoot const &xaml_root, glance::app::UpdateCheckResult result, bool automatic_prompt)
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
                dialog.Content(
                    box_value(glance::app::localize_format(L"UpdateAvailableMessage", {result.latest_version})));
                dialog.PrimaryButtonText(
                    glance::app::localize(installer_available ? L"UpdateDownloadAndInstall" : L"UpdateOpenRelease"));
                if (installer_available)
                {
                    dialog.SecondaryButtonText(glance::app::localize(L"UpdateOpenRelease"));
                }
                dialog.CloseButtonText(glance::app::localize(automatic_prompt ? L"UpdateSkipVersion" : L"Cancel"));
                dialog.DefaultButton(Controls::ContentDialogButton::Primary);
                break;
            case glance::app::UpdateCheckStatus::up_to_date:
                dialog.Title(box_value(glance::app::localize(L"UpdateUpToDateTitle")));
                dialog.Content(
                    box_value(glance::app::localize_format(L"UpdateUpToDateMessage", {GLANCE_VERSION_WSTRING})));
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
                if (installer_available && dialog_result == Controls::ContentDialogResult::Primary)
                {
                    co_return static_cast<std::int32_t>(UpdatePromptResult::download);
                }
                if ((!installer_available && dialog_result == Controls::ContentDialogResult::Primary) ||
                    dialog_result == Controls::ContentDialogResult::Secondary)
                {
                    static_cast<void>(co_await Windows::System::Launcher::LaunchUriAsync(
                        Windows::Foundation::Uri(safe_release_url(result.release_url))));
                    co_return static_cast<std::int32_t>(UpdatePromptResult::other);
                }
                if (automatic_prompt && dialog_result == Controls::ContentDialogResult::None)
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

} // namespace winrt::Glance::App::implementation

#include "pch.h"
#include <winrt/Microsoft.UI.Xaml.Markup.h>
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

namespace winrt::Glance::App::implementation
{
    void SettingsWindow::register_components_settings()
    {
        settings_registry_.register_page({L"components", L"", L"ComponentsNavigationItem.Content",
                                          L"ComponentsPageDescription.Text",
                                          glance::app::SettingsNavigationPosition::bottom});
        settings_registry_.register_section({L"DependencyGroupTitle", L"components", L"DependencyGroupTitle.Text", {}});
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"DependencyFolderLabel";
            definition.parent = L"DependencyGroupTitle";
            definition.name_key = L"DependencyFolderLabel.Text";
            definition.description_key = L"DependencyFolderDescription.Text";
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" x:Name="OpenDependenciesFolderButton" Content="Open folder" IsTabStop="False" AllowFocusOnInteraction="False" /> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"OpenDependenciesFolderButton", control);
                const auto weak = get_weak();
                OpenDependenciesFolderButton().Click([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->OpenDependenciesFolderButton_Click(sender, args);
                });
                settings_registry_.bind_text(L"OpenDependenciesFolderButton", L"OpenDependenciesFolderButton.Content",
                                             true);
                OpenDependenciesFolderButton().Content(
                    box_value(glance::app::localize(L"OpenDependenciesFolderButton.Content")));
                return control;
            };
            settings_registry_.bind_factory(L"OpenDependenciesFolderButton", [this] {
                settings_registry_.item(L"DependencyFolderLabel");
                return settings_registry_.control(L"OpenDependenciesFolderButton");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"DependencyGroupTitle.custom50";
            definition.parent = L"DependencyGroupTitle";
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<Expander xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" HorizontalAlignment="Stretch" HorizontalContentAlignment="Stretch" Padding="0">
<Expander.Header>
<TextBlock x:Name="DependencyStatusGroupTitle" Style="{StaticResource SettingsGroupTitleStyle}" Text="Dependency status" />
</Expander.Header>
<StackPanel x:Name="DependencyStatusList" />
</Expander> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"DependencyStatusGroupTitle",
                                        control.FindName(L"DependencyStatusGroupTitle").as<FrameworkElement>());
                settings_registry_.bind(L"DependencyStatusList",
                                        control.FindName(L"DependencyStatusList").as<FrameworkElement>());
                settings_registry_.bind_text(L"DependencyStatusGroupTitle", L"DependencyStatusGroupTitle.Text", false);
                DependencyStatusGroupTitle().Text(glance::app::localize(L"DependencyStatusGroupTitle.Text"));
                return control;
            };
            settings_registry_.bind_factory(L"DependencyStatusGroupTitle", [this] {
                settings_registry_.item(L"DependencyGroupTitle.custom50");
                return settings_registry_.control(L"DependencyStatusGroupTitle");
            });
            settings_registry_.bind_factory(L"DependencyStatusList", [this] {
                settings_registry_.item(L"DependencyGroupTitle.custom50");
                return settings_registry_.control(L"DependencyStatusList");
            });
            settings_registry_.register_custom_item(std::move(definition));
        }
        settings_registry_.register_section(
            {L"ComponentLocationGroupTitle", L"components", L"ComponentLocationGroupTitle.Text", {}});
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"ComponentFolderLabel";
            definition.parent = L"ComponentLocationGroupTitle";
            definition.name_key = L"ComponentFolderLabel.Text";
            definition.description_key = L"ComponentFolderDescription.Text";
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" x:Name="OpenComponentsFolderButton" Content="Open folder" IsTabStop="False" AllowFocusOnInteraction="False" /> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"OpenComponentsFolderButton", control);
                const auto weak = get_weak();
                OpenComponentsFolderButton().Click([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->OpenComponentsFolderButton_Click(sender, args);
                });
                settings_registry_.bind_text(L"OpenComponentsFolderButton", L"OpenComponentsFolderButton.Content",
                                             true);
                OpenComponentsFolderButton().Content(
                    box_value(glance::app::localize(L"OpenComponentsFolderButton.Content")));
                return control;
            };
            settings_registry_.bind_factory(L"OpenComponentsFolderButton", [this] {
                settings_registry_.item(L"ComponentFolderLabel");
                return settings_registry_.control(L"OpenComponentsFolderButton");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"ComponentStatusExpander";
            definition.parent = L"ComponentLocationGroupTitle";
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<Expander xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" x:Name="ComponentStatusExpander" HorizontalAlignment="Stretch" HorizontalContentAlignment="Stretch" Padding="0">
<Expander.Header>
<TextBlock x:Name="ComponentStatusGroupTitle" Style="{StaticResource SettingsGroupTitleStyle}" Text="Add-on component status" />
</Expander.Header>
<Grid>
<Grid x:Name="ComponentEmptyState" MinHeight="160">
<StackPanel HorizontalAlignment="Center" VerticalAlignment="Center" Spacing="8">
<FontIcon HorizontalAlignment="Center" FontSize="32" Glyph="" Opacity="0.65" />
<TextBlock x:Name="ComponentEmptyTitle" HorizontalAlignment="Center" FontWeight="SemiBold" Text="No components installed" />
<TextBlock x:Name="ComponentEmptyDescription" MaxWidth="360" HorizontalAlignment="Center" Style="{StaticResource SettingsDescriptionStyle}" Text="Install optional components to add support for more file formats" TextAlignment="Center" TextWrapping="Wrap" />
</StackPanel>
</Grid>
<Grid x:Name="ComponentStatusScroller" Visibility="Collapsed">
<StackPanel x:Name="ComponentStatusList" />
</Grid>
</Grid>
</Expander> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"ComponentStatusExpander", control);
                settings_registry_.bind(L"ComponentStatusGroupTitle",
                                        control.FindName(L"ComponentStatusGroupTitle").as<FrameworkElement>());
                settings_registry_.bind(L"ComponentEmptyState",
                                        control.FindName(L"ComponentEmptyState").as<FrameworkElement>());
                settings_registry_.bind(L"ComponentEmptyTitle",
                                        control.FindName(L"ComponentEmptyTitle").as<FrameworkElement>());
                settings_registry_.bind(L"ComponentEmptyDescription",
                                        control.FindName(L"ComponentEmptyDescription").as<FrameworkElement>());
                settings_registry_.bind(L"ComponentStatusScroller",
                                        control.FindName(L"ComponentStatusScroller").as<FrameworkElement>());
                settings_registry_.bind(L"ComponentStatusList",
                                        control.FindName(L"ComponentStatusList").as<FrameworkElement>());
                settings_registry_.bind_text(L"ComponentStatusGroupTitle", L"ComponentStatusGroupTitle.Text", false);
                ComponentStatusGroupTitle().Text(glance::app::localize(L"ComponentStatusGroupTitle.Text"));
                settings_registry_.bind_text(L"ComponentEmptyTitle", L"ComponentEmptyTitle.Text", false);
                ComponentEmptyTitle().Text(glance::app::localize(L"ComponentEmptyTitle.Text"));
                settings_registry_.bind_text(L"ComponentEmptyDescription", L"ComponentEmptyDescription.Text", false);
                ComponentEmptyDescription().Text(glance::app::localize(L"ComponentEmptyDescription.Text"));
                return control;
            };
            settings_registry_.bind_factory(L"ComponentStatusExpander", [this] {
                settings_registry_.item(L"ComponentStatusExpander");
                return settings_registry_.control(L"ComponentStatusExpander");
            });
            settings_registry_.bind_factory(L"ComponentStatusGroupTitle", [this] {
                settings_registry_.item(L"ComponentStatusExpander");
                return settings_registry_.control(L"ComponentStatusGroupTitle");
            });
            settings_registry_.bind_factory(L"ComponentEmptyState", [this] {
                settings_registry_.item(L"ComponentStatusExpander");
                return settings_registry_.control(L"ComponentEmptyState");
            });
            settings_registry_.bind_factory(L"ComponentEmptyTitle", [this] {
                settings_registry_.item(L"ComponentStatusExpander");
                return settings_registry_.control(L"ComponentEmptyTitle");
            });
            settings_registry_.bind_factory(L"ComponentEmptyDescription", [this] {
                settings_registry_.item(L"ComponentStatusExpander");
                return settings_registry_.control(L"ComponentEmptyDescription");
            });
            settings_registry_.bind_factory(L"ComponentStatusScroller", [this] {
                settings_registry_.item(L"ComponentStatusExpander");
                return settings_registry_.control(L"ComponentStatusScroller");
            });
            settings_registry_.bind_factory(L"ComponentStatusList", [this] {
                settings_registry_.item(L"ComponentStatusExpander");
                return settings_registry_.control(L"ComponentStatusList");
            });
            settings_registry_.register_custom_item(std::move(definition));
        }
        settings_registry_.register_section(
            {L"SourceLocationGroupTitle", L"components", L"SourceLocationGroupTitle.Text", {}});
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"SourceFolderLabel";
            definition.parent = L"SourceLocationGroupTitle";
            definition.name_key = L"SourceFolderLabel.Text";
            definition.description_key = L"SourceFolderDescription.Text";
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" x:Name="OpenSourcesFolderButton" Content="Open folder" IsTabStop="False" AllowFocusOnInteraction="False" /> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"OpenSourcesFolderButton", control);
                const auto weak = get_weak();
                OpenSourcesFolderButton().Click([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->OpenSourcesFolderButton_Click(sender, args);
                });
                settings_registry_.bind_text(L"OpenSourcesFolderButton", L"OpenSourcesFolderButton.Content", true);
                OpenSourcesFolderButton().Content(box_value(glance::app::localize(L"OpenSourcesFolderButton.Content")));
                return control;
            };
            settings_registry_.bind_factory(L"OpenSourcesFolderButton", [this] {
                settings_registry_.item(L"SourceFolderLabel");
                return settings_registry_.control(L"OpenSourcesFolderButton");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"SourceStatusExpander";
            definition.parent = L"SourceLocationGroupTitle";
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<Expander xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" x:Name="SourceStatusExpander" HorizontalAlignment="Stretch" HorizontalContentAlignment="Stretch" Padding="0">
<Expander.Header>
<TextBlock x:Name="SourceStatusGroupTitle" Style="{StaticResource SettingsGroupTitleStyle}" Text="Add-on source status" />
</Expander.Header>
<Grid>
<Grid x:Name="SourceEmptyState" MinHeight="120">
<StackPanel HorizontalAlignment="Center" VerticalAlignment="Center" Spacing="8">
<FontIcon HorizontalAlignment="Center" FontSize="32" Glyph="" Opacity="0.65" />
<TextBlock x:Name="SourceEmptyTitle" HorizontalAlignment="Center" FontWeight="SemiBold" Text="No sources installed" />
<TextBlock x:Name="SourceEmptyDescription" MaxWidth="360" HorizontalAlignment="Center" Style="{StaticResource SettingsDescriptionStyle}" Text="Install optional sources to preview files from supported applications" TextAlignment="Center" TextWrapping="Wrap" />
</StackPanel>
</Grid>
<Grid x:Name="SourceStatusScroller" Visibility="Collapsed">
<StackPanel x:Name="SourceStatusList" />
</Grid>
</Grid>
</Expander> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"SourceStatusExpander", control);
                settings_registry_.bind(L"SourceStatusGroupTitle",
                                        control.FindName(L"SourceStatusGroupTitle").as<FrameworkElement>());
                settings_registry_.bind(L"SourceEmptyState",
                                        control.FindName(L"SourceEmptyState").as<FrameworkElement>());
                settings_registry_.bind(L"SourceEmptyTitle",
                                        control.FindName(L"SourceEmptyTitle").as<FrameworkElement>());
                settings_registry_.bind(L"SourceEmptyDescription",
                                        control.FindName(L"SourceEmptyDescription").as<FrameworkElement>());
                settings_registry_.bind(L"SourceStatusScroller",
                                        control.FindName(L"SourceStatusScroller").as<FrameworkElement>());
                settings_registry_.bind(L"SourceStatusList",
                                        control.FindName(L"SourceStatusList").as<FrameworkElement>());
                settings_registry_.bind_text(L"SourceStatusGroupTitle", L"SourceStatusGroupTitle.Text", false);
                SourceStatusGroupTitle().Text(glance::app::localize(L"SourceStatusGroupTitle.Text"));
                settings_registry_.bind_text(L"SourceEmptyTitle", L"SourceEmptyTitle.Text", false);
                SourceEmptyTitle().Text(glance::app::localize(L"SourceEmptyTitle.Text"));
                settings_registry_.bind_text(L"SourceEmptyDescription", L"SourceEmptyDescription.Text", false);
                SourceEmptyDescription().Text(glance::app::localize(L"SourceEmptyDescription.Text"));
                return control;
            };
            settings_registry_.bind_factory(L"SourceStatusExpander", [this] {
                settings_registry_.item(L"SourceStatusExpander");
                return settings_registry_.control(L"SourceStatusExpander");
            });
            settings_registry_.bind_factory(L"SourceStatusGroupTitle", [this] {
                settings_registry_.item(L"SourceStatusExpander");
                return settings_registry_.control(L"SourceStatusGroupTitle");
            });
            settings_registry_.bind_factory(L"SourceEmptyState", [this] {
                settings_registry_.item(L"SourceStatusExpander");
                return settings_registry_.control(L"SourceEmptyState");
            });
            settings_registry_.bind_factory(L"SourceEmptyTitle", [this] {
                settings_registry_.item(L"SourceStatusExpander");
                return settings_registry_.control(L"SourceEmptyTitle");
            });
            settings_registry_.bind_factory(L"SourceEmptyDescription", [this] {
                settings_registry_.item(L"SourceStatusExpander");
                return settings_registry_.control(L"SourceEmptyDescription");
            });
            settings_registry_.bind_factory(L"SourceStatusScroller", [this] {
                settings_registry_.item(L"SourceStatusExpander");
                return settings_registry_.control(L"SourceStatusScroller");
            });
            settings_registry_.bind_factory(L"SourceStatusList", [this] {
                settings_registry_.item(L"SourceStatusExpander");
                return settings_registry_.control(L"SourceStatusList");
            });
            settings_registry_.register_custom_item(std::move(definition));
        }
    }

    winrt::fire_and_forget SettingsWindow::uninstall_dependency(std::wstring id)
    {
        const auto lifetime = get_strong();
        const apartment_context ui_thread;
        HRESULT result = S_OK;
        co_await resume_background();
        try
        {
            glance::app::dependencies::uninstall(id);
        }
        catch (...)
        {
            result = winrt::to_hresult();
        }
        co_await ui_thread;
        if (FAILED(result))
            lifetime->dependency_errors_[id] =
                result == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) ? L"DependencyInUse" : L"DependencyFailed";
        lifetime->refresh_component_statuses();
    }

    void SettingsWindow::request_source_statuses()
    {
        if (!settings_registry_.page_created(L"components"))
            return;
        if (source_status_request_callback_)
        {
            static_cast<void>(source_status_request_callback_(winrt::to_string(glance::app::current_ui_language())));
        }
    }

    void SettingsWindow::OpenDependenciesFolderButton_Click(IInspectable const &, RoutedEventArgs const &)
    {
        try
        {
            const auto path = glance::app::dependencies::storage_root();
            std::filesystem::create_directories(path);
            static_cast<void>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        }
        catch (...)
        {
        }
    }

    void SettingsWindow::OpenComponentsFolderButton_Click(IInspectable const &, RoutedEventArgs const &)
    {
        try
        {
            const auto path = glance::app::application_component_root();
            std::filesystem::create_directories(path);
            static_cast<void>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        }
        catch (...)
        {
        }
    }

    void SettingsWindow::OpenSourcesFolderButton_Click(IInspectable const &, RoutedEventArgs const &)
    {
        try
        {
            const auto path = glance::app::application_component_root().parent_path() / L"sources";
            std::filesystem::create_directories(path);
            static_cast<void>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        }
        catch (...)
        {
        }
    }
} // namespace winrt::Glance::App::implementation

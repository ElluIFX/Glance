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
    void SettingsWindow::register_window_settings()
    {
        settings_registry_.register_page({L"window", L"", L"WindowNavigationItem.Content",
                                          L"WindowPageDescription.Text", glance::app::SettingsNavigationPosition::top});
        settings_registry_.register_section(
            {L"WindowBehaviorSectionTitle", L"window", L"WindowBehaviorSectionTitle.Text", {}});
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"DefaultWindowSizeLabel";
            definition.parent = L"WindowBehaviorSectionTitle";
            definition.name_key = L"DefaultWindowSizeLabel.Text";
            definition.description_key = L"DefaultWindowSizeDescription.Text";
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<StackPanel xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" HorizontalAlignment="Right" VerticalAlignment="Center" Orientation="Horizontal" Spacing="8">
<NumberBox x:Name="DefaultWindowWidthNumberBox" Width="96" Minimum="480" Maximum="7680" SmallChange="10" SpinButtonPlacementMode="Compact" />
<TextBlock VerticalAlignment="Center" Text="×" />
<NumberBox x:Name="DefaultWindowHeightNumberBox" Width="96" Minimum="320" Maximum="4320" SmallChange="10" SpinButtonPlacementMode="Compact" />
</StackPanel> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"DefaultWindowWidthNumberBox",
                                        control.FindName(L"DefaultWindowWidthNumberBox").as<FrameworkElement>());
                settings_registry_.bind(L"DefaultWindowHeightNumberBox",
                                        control.FindName(L"DefaultWindowHeightNumberBox").as<FrameworkElement>());
                const auto weak = get_weak();
                DefaultWindowWidthNumberBox().Loaded([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->NumberBox_Loaded(sender, args);
                });
                DefaultWindowWidthNumberBox().ValueChanged([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->WindowNumberBox_ValueChanged(sender, args);
                });
                DefaultWindowHeightNumberBox().Loaded([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->NumberBox_Loaded(sender, args);
                });
                DefaultWindowHeightNumberBox().ValueChanged([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->WindowNumberBox_ValueChanged(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"DefaultWindowWidthNumberBox", [this] {
                settings_registry_.item(L"DefaultWindowSizeLabel");
                return settings_registry_.control(L"DefaultWindowWidthNumberBox");
            });
            settings_registry_.bind_factory(L"DefaultWindowHeightNumberBox", [this] {
                settings_registry_.item(L"DefaultWindowSizeLabel");
                return settings_registry_.control(L"DefaultWindowHeightNumberBox");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"RememberWindowSizeLabel";
            definition.refresh = [this] {
                ResetWindowSizesButton().Content(box_value(glance::app::localize(
                    size_reset_text_key_.empty() ? L"ResetWindowSizesButton.Content" : size_reset_text_key_)));
            };
            definition.parent = L"WindowBehaviorSectionTitle";
            definition.name_key = L"RememberWindowSizeLabel.Text";
            definition.description = [this] {
                return glance::app::SettingsText{RememberWindowSizeToggle().IsOn()
                                                     ? L"RememberWindowSizeEnabledDescription.Text"
                                                     : L"RememberWindowSizeDisabledDescription.Text",
                                                 {}};
            };
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<StackPanel xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" HorizontalAlignment="Right" VerticalAlignment="Center" Orientation="Horizontal" Spacing="12">
<Button x:Name="ResetWindowSizesButton" Content="Reset" IsTabStop="False" AllowFocusOnInteraction="False" />
<ToggleSwitch x:Name="RememberWindowSizeToggle" />
</StackPanel> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"ResetWindowSizesButton",
                                        control.FindName(L"ResetWindowSizesButton").as<FrameworkElement>());
                settings_registry_.bind(L"RememberWindowSizeToggle",
                                        control.FindName(L"RememberWindowSizeToggle").as<FrameworkElement>());
                const auto weak = get_weak();
                ResetWindowSizesButton().Click([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->ResetWindowSizesButton_Click(sender, args);
                });
                RememberWindowSizeToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->WindowPreferenceToggle_Toggled(sender, args);
                });
                ResetWindowSizesButton().Content(box_value(glance::app::localize(L"ResetWindowSizesButton.Content")));
                return control;
            };
            settings_registry_.bind_factory(L"ResetWindowSizesButton", [this] {
                settings_registry_.item(L"RememberWindowSizeLabel");
                return settings_registry_.control(L"ResetWindowSizesButton");
            });
            settings_registry_.bind_factory(L"RememberWindowSizeToggle", [this] {
                settings_registry_.item(L"RememberWindowSizeLabel");
                return settings_registry_.control(L"RememberWindowSizeToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"RememberWindowPositionLabel";
            definition.refresh = [this] {
                ResetWindowPositionsButton().Content(box_value(glance::app::localize(
                    position_reset_text_key_.empty() ? L"ResetWindowPositionsButton.Content" : position_reset_text_key_)));
            };
            definition.parent = L"WindowBehaviorSectionTitle";
            definition.name_key = L"RememberWindowPositionLabel.Text";
            definition.description = [this] {
                return glance::app::SettingsText{RememberWindowPositionToggle().IsOn()
                                                     ? L"RememberWindowPositionEnabledDescription.Text"
                                                     : L"RememberWindowPositionDisabledDescription.Text",
                                                 {}};
            };
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<StackPanel xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" HorizontalAlignment="Right" VerticalAlignment="Center" Orientation="Horizontal" Spacing="12">
<Button x:Name="ResetWindowPositionsButton" Content="Reset" IsTabStop="False" AllowFocusOnInteraction="False" />
<ToggleSwitch x:Name="RememberWindowPositionToggle" />
</StackPanel> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"ResetWindowPositionsButton",
                                        control.FindName(L"ResetWindowPositionsButton").as<FrameworkElement>());
                settings_registry_.bind(L"RememberWindowPositionToggle",
                                        control.FindName(L"RememberWindowPositionToggle").as<FrameworkElement>());
                const auto weak = get_weak();
                ResetWindowPositionsButton().Click([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->ResetWindowPositionsButton_Click(sender, args);
                });
                RememberWindowPositionToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->WindowPreferenceToggle_Toggled(sender, args);
                });
                ResetWindowPositionsButton().Content(
                    box_value(glance::app::localize(L"ResetWindowPositionsButton.Content")));
                return control;
            };
            settings_registry_.bind_factory(L"ResetWindowPositionsButton", [this] {
                settings_registry_.item(L"RememberWindowPositionLabel");
                return settings_registry_.control(L"ResetWindowPositionsButton");
            });
            settings_registry_.bind_factory(L"RememberWindowPositionToggle", [this] {
                settings_registry_.item(L"RememberWindowPositionLabel");
                return settings_registry_.control(L"RememberWindowPositionToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"DoubleClickFullscreenLabel";
            definition.parent = L"WindowBehaviorSectionTitle";
            definition.name_key = L"DoubleClickFullscreenLabel.Text";
            definition.description = [this] {
                return glance::app::SettingsText{DoubleClickFullscreenToggle().IsOn()
                                                     ? L"DoubleClickFullscreenEnabledDescription.Text"
                                                     : L"DoubleClickFullscreenDisabledDescription.Text",
                                                 {}};
            };
            definition.create_control = [this] {
                auto control = glance::app::make_settings_toggle();
                settings_registry_.bind(L"DoubleClickFullscreenToggle", control);
                const auto weak = get_weak();
                DoubleClickFullscreenToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->WindowPreferenceToggle_Toggled(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"DoubleClickFullscreenToggle", [this] {
                settings_registry_.item(L"DoubleClickFullscreenLabel");
                return settings_registry_.control(L"DoubleClickFullscreenToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"RightClickCloseLabel";
            definition.parent = L"WindowBehaviorSectionTitle";
            definition.name_key = L"RightClickCloseLabel.Text";
            definition.description = [this] {
                return glance::app::SettingsText{RightClickCloseToggle().IsOn()
                                                     ? L"RightClickCloseEnabledDescription.Text"
                                                     : L"RightClickCloseDisabledDescription.Text",
                                                 {}};
            };
            definition.create_control = [this] {
                auto control = glance::app::make_settings_toggle();
                settings_registry_.bind(L"RightClickCloseToggle", control);
                const auto weak = get_weak();
                RightClickCloseToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->WindowPreferenceToggle_Toggled(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"RightClickCloseToggle", [this] {
                settings_registry_.item(L"RightClickCloseLabel");
                return settings_registry_.control(L"RightClickCloseToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
        settings_registry_.register_section(
            {L"AdaptiveMediaSizeSectionTitle", L"window", L"AdaptiveMediaSizeSectionTitle.Text", {}});
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"AutoFitWindowSizeLabel";
            definition.parent = L"AdaptiveMediaSizeSectionTitle";
            definition.name_key = L"AutoFitWindowSizeLabel.Text";
            definition.description = [this] {
                return glance::app::SettingsText{AutoFitWindowSizeToggle().IsOn()
                                                     ? L"AutoFitWindowSizeEnabledDescription.Text"
                                                     : L"AutoFitWindowSizeDisabledDescription.Text",
                                                 {}};
            };
            definition.create_control = [this] {
                auto control = glance::app::make_settings_toggle();
                settings_registry_.bind(L"AutoFitWindowSizeToggle", control);
                const auto weak = get_weak();
                AutoFitWindowSizeToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->WindowPreferenceToggle_Toggled(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"AutoFitWindowSizeToggle", [this] {
                settings_registry_.item(L"AutoFitWindowSizeLabel");
                return settings_registry_.control(L"AutoFitWindowSizeToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"PauseAutoFitWhenTopmostLabel";
            definition.parent = L"AdaptiveMediaSizeSectionTitle";
            definition.name_key = L"PauseAutoFitWhenTopmostLabel.Text";
            definition.description = [this] {
                return glance::app::SettingsText{PauseAutoFitWhenTopmostToggle().IsOn()
                                                     ? L"PauseAutoFitWhenTopmostEnabledDescription.Text"
                                                     : L"PauseAutoFitWhenTopmostDisabledDescription.Text",
                                                 {}};
            };
            definition.visible = [this] { return AutoFitWindowSizeToggle().IsOn(); };
            definition.create_control = [this] {
                auto control = glance::app::make_settings_toggle();
                settings_registry_.bind(L"PauseAutoFitWhenTopmostToggle", control);
                const auto weak = get_weak();
                PauseAutoFitWhenTopmostToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->WindowPreferenceToggle_Toggled(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"PauseAutoFitWhenTopmostToggle", [this] {
                settings_registry_.item(L"PauseAutoFitWhenTopmostLabel");
                return settings_registry_.control(L"PauseAutoFitWhenTopmostToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"PauseAutoFitInGalleryLabel";
            definition.parent = L"AdaptiveMediaSizeSectionTitle";
            definition.name_key = L"PauseAutoFitInGalleryLabel.Text";
            definition.description = [this] {
                return glance::app::SettingsText{PauseAutoFitInGalleryToggle().IsOn()
                                                     ? L"PauseAutoFitInGalleryEnabledDescription.Text"
                                                     : L"PauseAutoFitInGalleryDisabledDescription.Text",
                                                 {}};
            };
            definition.visible = [this] { return AutoFitWindowSizeToggle().IsOn(); };
            definition.create_control = [this] {
                auto control = glance::app::make_settings_toggle();
                settings_registry_.bind(L"PauseAutoFitInGalleryToggle", control);
                const auto weak = get_weak();
                PauseAutoFitInGalleryToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->WindowPreferenceToggle_Toggled(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"PauseAutoFitInGalleryToggle", [this] {
                settings_registry_.item(L"PauseAutoFitInGalleryLabel");
                return settings_registry_.control(L"PauseAutoFitInGalleryToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"ShowAfterAutoFitLabel";
            definition.parent = L"AdaptiveMediaSizeSectionTitle";
            definition.name_key = L"ShowAfterAutoFitLabel.Text";
            definition.description = [this] {
                return glance::app::SettingsText{ShowAfterAutoFitToggle().IsOn()
                                                     ? L"ShowAfterAutoFitEnabledDescription.Text"
                                                     : L"ShowAfterAutoFitDisabledDescription.Text",
                                                 {}};
            };
            definition.visible = [this] { return AutoFitWindowSizeToggle().IsOn(); };
            definition.create_control = [this] {
                auto control = glance::app::make_settings_toggle();
                settings_registry_.bind(L"ShowAfterAutoFitToggle", control);
                const auto weak = get_weak();
                ShowAfterAutoFitToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->WindowPreferenceToggle_Toggled(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"ShowAfterAutoFitToggle", [this] {
                settings_registry_.item(L"ShowAfterAutoFitLabel");
                return settings_registry_.control(L"ShowAfterAutoFitToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"DynamicAutoFitLabel";
            definition.parent = L"AdaptiveMediaSizeSectionTitle";
            definition.name_key = L"DynamicAutoFitLabel.Text";
            definition.description = [this] {
                return glance::app::SettingsText{DynamicAutoFitToggle().IsOn()
                                                     ? L"DynamicAutoFitEnabledDescription.Text"
                                                     : L"DynamicAutoFitDisabledDescription.Text",
                                                 {}};
            };
            definition.visible = [this] { return AutoFitWindowSizeToggle().IsOn(); };
            definition.create_control = [this] {
                auto control = glance::app::make_settings_toggle();
                settings_registry_.bind(L"DynamicAutoFitToggle", control);
                const auto weak = get_weak();
                DynamicAutoFitToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->WindowPreferenceToggle_Toggled(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"DynamicAutoFitToggle", [this] {
                settings_registry_.item(L"DynamicAutoFitLabel");
                return settings_registry_.control(L"DynamicAutoFitToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"AdaptiveSizeRangeLabel";
            definition.parent = L"AdaptiveMediaSizeSectionTitle";
            definition.name_key = L"AdaptiveSizeRangeLabel.Text";
            definition.description_key = L"AdaptiveSizeRangeDescription.Text";
            definition.visible = [this] { return AutoFitWindowSizeToggle().IsOn(); };
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<StackPanel xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" HorizontalAlignment="Right" VerticalAlignment="Center" Orientation="Horizontal" Spacing="6">
<NumberBox x:Name="AdaptiveMinimumPercentNumberBox" Width="82" Minimum="10" Maximum="100" SmallChange="5" SpinButtonPlacementMode="Compact" />
<TextBlock VerticalAlignment="Center" Text="–" />
<NumberBox x:Name="AdaptiveMaximumPercentNumberBox" Width="82" Minimum="10" Maximum="100" SmallChange="5" SpinButtonPlacementMode="Compact" />
<TextBlock VerticalAlignment="Center" Text="%" />
</StackPanel> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"AdaptiveMinimumPercentNumberBox",
                                        control.FindName(L"AdaptiveMinimumPercentNumberBox").as<FrameworkElement>());
                settings_registry_.bind(L"AdaptiveMaximumPercentNumberBox",
                                        control.FindName(L"AdaptiveMaximumPercentNumberBox").as<FrameworkElement>());
                const auto weak = get_weak();
                AdaptiveMinimumPercentNumberBox().Loaded([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->NumberBox_Loaded(sender, args);
                });
                AdaptiveMinimumPercentNumberBox().ValueChanged([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->WindowNumberBox_ValueChanged(sender, args);
                });
                AdaptiveMaximumPercentNumberBox().Loaded([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->NumberBox_Loaded(sender, args);
                });
                AdaptiveMaximumPercentNumberBox().ValueChanged([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->WindowNumberBox_ValueChanged(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"AdaptiveMinimumPercentNumberBox", [this] {
                settings_registry_.item(L"AdaptiveSizeRangeLabel");
                return settings_registry_.control(L"AdaptiveMinimumPercentNumberBox");
            });
            settings_registry_.bind_factory(L"AdaptiveMaximumPercentNumberBox", [this] {
                settings_registry_.item(L"AdaptiveSizeRangeLabel");
                return settings_registry_.control(L"AdaptiveMaximumPercentNumberBox");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"AutoFitIgnoredExtensionsLabel";
            definition.parent = L"AdaptiveMediaSizeSectionTitle";
            definition.name_key = L"AutoFitIgnoredExtensionsLabel.Text";
            definition.description_key = L"AutoFitIgnoredExtensionsDescription.Text";
            definition.visible = [this] { return AutoFitWindowSizeToggle().IsOn(); };
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<TextBox xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" x:Name="AutoFitIgnoredExtensionsTextBox" HorizontalAlignment="Stretch" VerticalAlignment="Center" PlaceholderText=".gif; webp; mp4" Width="220" MinWidth="0" /> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"AutoFitIgnoredExtensionsTextBox", control);
                const auto weak = get_weak();
                AutoFitIgnoredExtensionsTextBox().TextChanged([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->AutoFitIgnoredExtensionsTextBox_TextChanged(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"AutoFitIgnoredExtensionsTextBox", [this] {
                settings_registry_.item(L"AutoFitIgnoredExtensionsLabel");
                return settings_registry_.control(L"AutoFitIgnoredExtensionsTextBox");
            });
            settings_registry_.register_item(std::move(definition));
        }
    }

    void SettingsWindow::WindowPreferenceToggle_Toggled(IInspectable const &, RoutedEventArgs const &)
    {
        refresh_toggle_descriptions();
        if (!initializing_)
        {
            window_preferences_.remember_size = RememberWindowSizeToggle().IsOn();
            window_preferences_.auto_fit_media = AutoFitWindowSizeToggle().IsOn();
            window_preferences_.pause_auto_fit_when_topmost = PauseAutoFitWhenTopmostToggle().IsOn();
            window_preferences_.pause_auto_fit_in_gallery = PauseAutoFitInGalleryToggle().IsOn();
            window_preferences_.show_after_auto_fit = ShowAfterAutoFitToggle().IsOn();
            window_preferences_.dynamic_auto_fit = DynamicAutoFitToggle().IsOn();
            window_preferences_.remember_position = RememberWindowPositionToggle().IsOn();
            window_preferences_.double_click_fullscreen = DoubleClickFullscreenToggle().IsOn();
            window_preferences_.right_click_close = RightClickCloseToggle().IsOn();
            update_auto_fit_dependency(true);
            glance::app::save_window_preferences(window_preferences_);
            if (window_preferences_changed_callback_)
            {
                window_preferences_changed_callback_();
            }
        }
    }

    void SettingsWindow::WindowNumberBox_ValueChanged(IInspectable const &sender,
                                                      Controls::NumberBoxValueChangedEventArgs const &)
    {
        if (initializing_)
        {
            return;
        }

        const double width = DefaultWindowWidthNumberBox().Value();
        const double height = DefaultWindowHeightNumberBox().Value();
        const double adaptive_minimum = AdaptiveMinimumPercentNumberBox().Value();
        const double adaptive_maximum = AdaptiveMaximumPercentNumberBox().Value();
        if (!std::isfinite(width) || !std::isfinite(height) || !std::isfinite(adaptive_minimum) ||
            !std::isfinite(adaptive_maximum))
        {
            initializing_ = true;
            DefaultWindowWidthNumberBox().Value(window_preferences_.default_width);
            DefaultWindowHeightNumberBox().Value(window_preferences_.default_height);
            AdaptiveMinimumPercentNumberBox().Value(window_preferences_.adaptive_minimum_percent);
            AdaptiveMaximumPercentNumberBox().Value(window_preferences_.adaptive_maximum_percent);
            initializing_ = false;
            return;
        }

        window_preferences_.default_width = static_cast<std::uint32_t>(std::clamp(std::lround(width), 480L, 7680L));
        window_preferences_.default_height = static_cast<std::uint32_t>(std::clamp(std::lround(height), 320L, 4320L));
        auto minimum_percent = static_cast<std::uint32_t>(std::clamp(std::lround(adaptive_minimum), 10L, 100L));
        auto maximum_percent = static_cast<std::uint32_t>(std::clamp(std::lround(adaptive_maximum), 10L, 100L));
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
    }

    void SettingsWindow::AutoFitIgnoredExtensionsTextBox_TextChanged(IInspectable const &,
                                                                     Controls::TextChangedEventArgs const &)
    {
        if (initializing_)
        {
            return;
        }
        window_preferences_.auto_fit_ignored_extensions = AutoFitIgnoredExtensionsTextBox().Text().c_str();
        glance::app::save_window_preferences(window_preferences_);
    }

    void SettingsWindow::update_auto_fit_dependency(bool animate)
    {
        settings_registry_.refresh(animate);
    }

    void SettingsWindow::ResetWindowSizesButton_Click(IInspectable const &, RoutedEventArgs const &)
    {
        show_window_reset_result(true, glance::app::clear_window_sizes());
    }

    void SettingsWindow::ResetWindowPositionsButton_Click(IInspectable const &, RoutedEventArgs const &)
    {
        show_window_reset_result(false, glance::app::clear_window_positions());
    }

    void SettingsWindow::show_window_reset_result(bool sizes, bool succeeded)
    {
        auto &timer = sizes ? size_reset_timer_ : position_reset_timer_;
        auto &key = sizes ? size_reset_text_key_ : position_reset_text_key_;
        key = succeeded ? L"WindowResetCompleted" : L"WindowResetFailed";
        if (!timer)
        {
            timer = DispatcherTimer();
            timer.Interval(std::chrono::seconds(2));
            timer.Tick([weak = get_weak(), sizes](auto const &sender, auto const &) {
                sender.template as<DispatcherTimer>().Stop();
                if (const auto self = weak.get())
                {
                    (sizes ? self->size_reset_text_key_ : self->position_reset_text_key_).clear();
                    self->settings_registry_.refresh_item(
                        sizes ? L"RememberWindowSizeLabel" : L"RememberWindowPositionLabel");
                }
            });
        }
        timer.Stop();
        settings_registry_.refresh_item(sizes ? L"RememberWindowSizeLabel" : L"RememberWindowPositionLabel");
        timer.Start();
    }
} // namespace winrt::Glance::App::implementation

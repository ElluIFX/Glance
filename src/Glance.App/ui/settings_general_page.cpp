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
    void SettingsWindow::register_general_settings()
    {
        settings_registry_.register_page({L"general", L"", L"GeneralNavigationItem.Content",
                                          L"GeneralPageDescription.Text",
                                          glance::app::SettingsNavigationPosition::top});
        settings_registry_.register_section({L"AppearanceGroupTitle", L"general", L"AppearanceGroupTitle.Text", {}});
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"LanguageLabel";
            definition.parent = L"AppearanceGroupTitle";
            definition.name_key = L"LanguageLabel.Text";
            definition.icon = L"";
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<ComboBox xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" x:Name="LanguageComboBox" HorizontalAlignment="Stretch" VerticalAlignment="Center" Width="220" MinWidth="0">
<ComboBoxItem x:Name="EnglishLanguageItem" Content="English" Tag="en-US" />
<ComboBoxItem x:Name="ChineseLanguageItem" Content="简体中文" Tag="zh-CN" />
</ComboBox> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"LanguageComboBox", control);
                settings_registry_.bind(L"EnglishLanguageItem",
                                        control.FindName(L"EnglishLanguageItem").as<FrameworkElement>());
                settings_registry_.bind(L"ChineseLanguageItem",
                                        control.FindName(L"ChineseLanguageItem").as<FrameworkElement>());
                const auto weak = get_weak();
                LanguageComboBox().SelectionChanged([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->AppearanceComboBox_SelectionChanged(sender, args);
                });
                settings_registry_.bind_text(L"EnglishLanguageItem", L"EnglishLanguageItem.Content", true);
                EnglishLanguageItem().Content(box_value(glance::app::localize(L"EnglishLanguageItem.Content")));
                settings_registry_.bind_text(L"ChineseLanguageItem", L"ChineseLanguageItem.Content", true);
                ChineseLanguageItem().Content(box_value(glance::app::localize(L"ChineseLanguageItem.Content")));
                return control;
            };
            settings_registry_.bind_factory(L"LanguageComboBox", [this] {
                settings_registry_.item(L"LanguageLabel");
                return settings_registry_.control(L"LanguageComboBox");
            });
            settings_registry_.bind_factory(L"EnglishLanguageItem", [this] {
                settings_registry_.item(L"LanguageLabel");
                return settings_registry_.control(L"EnglishLanguageItem");
            });
            settings_registry_.bind_factory(L"ChineseLanguageItem", [this] {
                settings_registry_.item(L"LanguageLabel");
                return settings_registry_.control(L"ChineseLanguageItem");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"ThemeLabel";
            definition.parent = L"AppearanceGroupTitle";
            definition.name_key = L"ThemeLabel.Text";
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<ComboBox xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" x:Name="ThemeComboBox" HorizontalAlignment="Stretch" VerticalAlignment="Center" Width="220" MinWidth="0" /> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"ThemeComboBox", control);
                settings_registry_.bind_choices(
                    L"ThemeComboBox",
                    {L"ThemeSystemItem.Content", L"ThemeLightItem.Content", L"ThemeDarkItem.Content"});
                const auto weak = get_weak();
                ThemeComboBox().SelectionChanged([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->AppearanceComboBox_SelectionChanged(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"ThemeComboBox", [this] {
                settings_registry_.item(L"ThemeLabel");
                return settings_registry_.control(L"ThemeComboBox");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"AccentColorLabel";
            definition.parent = L"AppearanceGroupTitle";
            definition.name_key = L"AccentColorLabel.Text";
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<ComboBox xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" x:Name="AccentComboBox" HorizontalAlignment="Stretch" VerticalAlignment="Center" Width="220" MinWidth="0">
<ComboBoxItem>
<StackPanel Orientation="Horizontal" Spacing="8">
<Ellipse Width="12" Height="12" Fill="{ThemeResource AccentFillColorDefaultBrush}" />
<TextBlock x:Name="AccentSystemText" Text="System" />
</StackPanel>
</ComboBoxItem>
<ComboBoxItem>
<StackPanel Orientation="Horizontal" Spacing="8">
<Ellipse Width="12" Height="12" Fill="#0078D4" />
<TextBlock x:Name="AccentBlueText" Text="Blue" />
</StackPanel>
</ComboBoxItem>
<ComboBoxItem>
<StackPanel Orientation="Horizontal" Spacing="8">
<Ellipse Width="12" Height="12" Fill="#008272" />
<TextBlock x:Name="AccentTealText" Text="Teal" />
</StackPanel>
</ComboBoxItem>
<ComboBoxItem>
<StackPanel Orientation="Horizontal" Spacing="8">
<Ellipse Width="12" Height="12" Fill="#107C10" />
<TextBlock x:Name="AccentGreenText" Text="Green" />
</StackPanel>
</ComboBoxItem>
<ComboBoxItem>
<StackPanel Orientation="Horizontal" Spacing="8">
<Ellipse Width="12" Height="12" Fill="#CA5010" />
<TextBlock x:Name="AccentOrangeText" Text="Orange" />
</StackPanel>
</ComboBoxItem>
<ComboBoxItem>
<StackPanel Orientation="Horizontal" Spacing="8">
<Ellipse Width="12" Height="12" Fill="#D13438" />
<TextBlock x:Name="AccentRedText" Text="Red" />
</StackPanel>
</ComboBoxItem>
<ComboBoxItem>
<StackPanel Orientation="Horizontal" Spacing="8">
<Ellipse Width="12" Height="12" Fill="#E3008C" />
<TextBlock x:Name="AccentPinkText" Text="Pink" />
</StackPanel>
</ComboBoxItem>
<ComboBoxItem>
<StackPanel Orientation="Horizontal" Spacing="8">
<Ellipse Width="12" Height="12" Fill="#5C2D91" />
<TextBlock x:Name="AccentPurpleText" Text="Purple" />
</StackPanel>
</ComboBoxItem>
</ComboBox> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"AccentComboBox", control);
                settings_registry_.bind(L"AccentSystemText",
                                        control.FindName(L"AccentSystemText").as<FrameworkElement>());
                settings_registry_.bind(L"AccentBlueText", control.FindName(L"AccentBlueText").as<FrameworkElement>());
                settings_registry_.bind(L"AccentTealText", control.FindName(L"AccentTealText").as<FrameworkElement>());
                settings_registry_.bind(L"AccentGreenText",
                                        control.FindName(L"AccentGreenText").as<FrameworkElement>());
                settings_registry_.bind(L"AccentOrangeText",
                                        control.FindName(L"AccentOrangeText").as<FrameworkElement>());
                settings_registry_.bind(L"AccentRedText", control.FindName(L"AccentRedText").as<FrameworkElement>());
                settings_registry_.bind(L"AccentPinkText", control.FindName(L"AccentPinkText").as<FrameworkElement>());
                settings_registry_.bind(L"AccentPurpleText",
                                        control.FindName(L"AccentPurpleText").as<FrameworkElement>());
                const auto weak = get_weak();
                AccentComboBox().SelectionChanged([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->AppearanceComboBox_SelectionChanged(sender, args);
                });
                settings_registry_.bind_text(L"AccentSystemText", L"AccentSystemText.Text", false);
                AccentSystemText().Text(glance::app::localize(L"AccentSystemText.Text"));
                settings_registry_.bind_text(L"AccentBlueText", L"AccentBlueText.Text", false);
                AccentBlueText().Text(glance::app::localize(L"AccentBlueText.Text"));
                settings_registry_.bind_text(L"AccentTealText", L"AccentTealText.Text", false);
                AccentTealText().Text(glance::app::localize(L"AccentTealText.Text"));
                settings_registry_.bind_text(L"AccentGreenText", L"AccentGreenText.Text", false);
                AccentGreenText().Text(glance::app::localize(L"AccentGreenText.Text"));
                settings_registry_.bind_text(L"AccentOrangeText", L"AccentOrangeText.Text", false);
                AccentOrangeText().Text(glance::app::localize(L"AccentOrangeText.Text"));
                settings_registry_.bind_text(L"AccentRedText", L"AccentRedText.Text", false);
                AccentRedText().Text(glance::app::localize(L"AccentRedText.Text"));
                settings_registry_.bind_text(L"AccentPinkText", L"AccentPinkText.Text", false);
                AccentPinkText().Text(glance::app::localize(L"AccentPinkText.Text"));
                settings_registry_.bind_text(L"AccentPurpleText", L"AccentPurpleText.Text", false);
                AccentPurpleText().Text(glance::app::localize(L"AccentPurpleText.Text"));
                return control;
            };
            settings_registry_.bind_factory(L"AccentComboBox", [this] {
                settings_registry_.item(L"AccentColorLabel");
                return settings_registry_.control(L"AccentComboBox");
            });
            settings_registry_.bind_factory(L"AccentSystemText", [this] {
                settings_registry_.item(L"AccentColorLabel");
                return settings_registry_.control(L"AccentSystemText");
            });
            settings_registry_.bind_factory(L"AccentBlueText", [this] {
                settings_registry_.item(L"AccentColorLabel");
                return settings_registry_.control(L"AccentBlueText");
            });
            settings_registry_.bind_factory(L"AccentTealText", [this] {
                settings_registry_.item(L"AccentColorLabel");
                return settings_registry_.control(L"AccentTealText");
            });
            settings_registry_.bind_factory(L"AccentGreenText", [this] {
                settings_registry_.item(L"AccentColorLabel");
                return settings_registry_.control(L"AccentGreenText");
            });
            settings_registry_.bind_factory(L"AccentOrangeText", [this] {
                settings_registry_.item(L"AccentColorLabel");
                return settings_registry_.control(L"AccentOrangeText");
            });
            settings_registry_.bind_factory(L"AccentRedText", [this] {
                settings_registry_.item(L"AccentColorLabel");
                return settings_registry_.control(L"AccentRedText");
            });
            settings_registry_.bind_factory(L"AccentPinkText", [this] {
                settings_registry_.item(L"AccentColorLabel");
                return settings_registry_.control(L"AccentPinkText");
            });
            settings_registry_.bind_factory(L"AccentPurpleText", [this] {
                settings_registry_.item(L"AccentColorLabel");
                return settings_registry_.control(L"AccentPurpleText");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"AcrylicMaterialLabel";
            definition.parent = L"AppearanceGroupTitle";
            definition.name_key = L"AcrylicMaterialLabel.Text";
            definition.visible = [this] { return glance::app::acrylic_material_supported(); };
            definition.create_control = [this] {
                auto control = glance::app::make_settings_toggle();
                settings_registry_.bind(L"AcrylicMaterialToggle", control);
                const auto weak = get_weak();
                AcrylicMaterialToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->AcrylicMaterialToggle_Toggled(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"AcrylicMaterialToggle", [this] {
                settings_registry_.item(L"AcrylicMaterialLabel");
                return settings_registry_.control(L"AcrylicMaterialToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"AcrylicOpacityLabel";
            definition.parent = L"AppearanceGroupTitle";
            definition.name_key = L"AcrylicOpacityLabel.Text";
            definition.visible = [this] {
                return glance::app::acrylic_material_supported() && AcrylicMaterialToggle().IsOn();
            };
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<Slider xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" x:Name="AcrylicOpacitySlider" HorizontalAlignment="Stretch" Maximum="100" Minimum="10" StepFrequency="1" Width="220" MinWidth="0" /> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"AcrylicOpacitySlider", control);
                const auto weak = get_weak();
                AcrylicOpacitySlider().ValueChanged([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->AcrylicOpacitySlider_ValueChanged(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"AcrylicOpacitySlider", [this] {
                settings_registry_.item(L"AcrylicOpacityLabel");
                return settings_registry_.control(L"AcrylicOpacitySlider");
            });
            settings_registry_.register_item(std::move(definition));
        }
        settings_registry_.register_section({L"StartupGroupTitle", L"general", L"StartupGroupTitle.Text", {}});
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"LaunchTitle";
            definition.parent = L"StartupGroupTitle";
            definition.name_key = L"LaunchTitle.Text";
            definition.description = [this] {
                return glance::app::SettingsText{LaunchAtSignInToggle().IsOn() ? L"LaunchEnabledDescription.Text"
                                                                               : L"LaunchDisabledDescription.Text",
                                                 {}};
            };
            definition.create_control = [this] {
                auto control = glance::app::make_settings_toggle();
                settings_registry_.bind(L"LaunchAtSignInToggle", control);
                const auto weak = get_weak();
                LaunchAtSignInToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->LaunchAtSignInToggle_Toggled(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"LaunchAtSignInToggle", [this] {
                settings_registry_.item(L"LaunchTitle");
                return settings_registry_.control(L"LaunchAtSignInToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
        settings_registry_.register_section({L"UpdateGroupTitle", L"general", L"UpdateGroupTitle.Text", {}});
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"AutomaticUpdateCheckLabel";
            definition.parent = L"UpdateGroupTitle";
            definition.name_key = L"AutomaticUpdateCheckLabel.Text";
            definition.description = [this] {
                return glance::app::SettingsText{AutomaticUpdateCheckToggle().IsOn()
                                                     ? L"AutomaticUpdateCheckEnabledDescription.Text"
                                                     : L"AutomaticUpdateCheckDisabledDescription.Text",
                                                 {}};
            };
            definition.create_control = [this] {
                auto control = glance::app::make_settings_toggle();
                settings_registry_.bind(L"AutomaticUpdateCheckToggle", control);
                const auto weak = get_weak();
                AutomaticUpdateCheckToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->AutomaticUpdateCheckToggle_Toggled(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"AutomaticUpdateCheckToggle", [this] {
                settings_registry_.item(L"AutomaticUpdateCheckLabel");
                return settings_registry_.control(L"AutomaticUpdateCheckToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"UpdateCheckFrequencyLabel";
            definition.parent = L"UpdateGroupTitle";
            definition.name_key = L"UpdateCheckFrequencyLabel.Text";
            definition.visible = [this] { return AutomaticUpdateCheckToggle().IsOn(); };
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<ComboBox xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" x:Name="UpdateCheckFrequencyComboBox" HorizontalAlignment="Stretch" Width="220" MinWidth="0">
<ComboBoxItem x:Name="UpdateFrequencyHourlyItem" Content="Hourly" />
<ComboBoxItem x:Name="UpdateFrequencyDailyItem" Content="Daily" />
<ComboBoxItem x:Name="UpdateFrequencyWeeklyItem" Content="Weekly" />
<ComboBoxItem x:Name="UpdateFrequencyMonthlyItem" Content="Monthly" />
</ComboBox> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"UpdateCheckFrequencyComboBox", control);
                settings_registry_.bind(L"UpdateFrequencyHourlyItem",
                                        control.FindName(L"UpdateFrequencyHourlyItem").as<FrameworkElement>());
                settings_registry_.bind(L"UpdateFrequencyDailyItem",
                                        control.FindName(L"UpdateFrequencyDailyItem").as<FrameworkElement>());
                settings_registry_.bind(L"UpdateFrequencyWeeklyItem",
                                        control.FindName(L"UpdateFrequencyWeeklyItem").as<FrameworkElement>());
                settings_registry_.bind(L"UpdateFrequencyMonthlyItem",
                                        control.FindName(L"UpdateFrequencyMonthlyItem").as<FrameworkElement>());
                const auto weak = get_weak();
                UpdateCheckFrequencyComboBox().SelectionChanged([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->UpdateCheckFrequencyComboBox_SelectionChanged(sender, args);
                });
                settings_registry_.bind_text(L"UpdateFrequencyHourlyItem", L"UpdateFrequencyHourlyItem.Content", true);
                UpdateFrequencyHourlyItem().Content(
                    box_value(glance::app::localize(L"UpdateFrequencyHourlyItem.Content")));
                settings_registry_.bind_text(L"UpdateFrequencyDailyItem", L"UpdateFrequencyDailyItem.Content", true);
                UpdateFrequencyDailyItem().Content(
                    box_value(glance::app::localize(L"UpdateFrequencyDailyItem.Content")));
                settings_registry_.bind_text(L"UpdateFrequencyWeeklyItem", L"UpdateFrequencyWeeklyItem.Content", true);
                UpdateFrequencyWeeklyItem().Content(
                    box_value(glance::app::localize(L"UpdateFrequencyWeeklyItem.Content")));
                settings_registry_.bind_text(L"UpdateFrequencyMonthlyItem", L"UpdateFrequencyMonthlyItem.Content",
                                             true);
                UpdateFrequencyMonthlyItem().Content(
                    box_value(glance::app::localize(L"UpdateFrequencyMonthlyItem.Content")));
                return control;
            };
            settings_registry_.bind_factory(L"UpdateCheckFrequencyComboBox", [this] {
                settings_registry_.item(L"UpdateCheckFrequencyLabel");
                return settings_registry_.control(L"UpdateCheckFrequencyComboBox");
            });
            settings_registry_.bind_factory(L"UpdateFrequencyHourlyItem", [this] {
                settings_registry_.item(L"UpdateCheckFrequencyLabel");
                return settings_registry_.control(L"UpdateFrequencyHourlyItem");
            });
            settings_registry_.bind_factory(L"UpdateFrequencyDailyItem", [this] {
                settings_registry_.item(L"UpdateCheckFrequencyLabel");
                return settings_registry_.control(L"UpdateFrequencyDailyItem");
            });
            settings_registry_.bind_factory(L"UpdateFrequencyWeeklyItem", [this] {
                settings_registry_.item(L"UpdateCheckFrequencyLabel");
                return settings_registry_.control(L"UpdateFrequencyWeeklyItem");
            });
            settings_registry_.bind_factory(L"UpdateFrequencyMonthlyItem", [this] {
                settings_registry_.item(L"UpdateCheckFrequencyLabel");
                return settings_registry_.control(L"UpdateFrequencyMonthlyItem");
            });
            settings_registry_.register_item(std::move(definition));
        }
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

    void SettingsWindow::LaunchAtSignInToggle_Toggled(IInspectable const &, RoutedEventArgs const &)
    {
        refresh_toggle_descriptions();
        if (!initializing_)
        {
            set_launch_at_sign_in(LaunchAtSignInToggle().IsOn());
        }
    }

    void SettingsWindow::AutomaticUpdateCheckToggle_Toggled(IInspectable const &, RoutedEventArgs const &)
    {
        refresh_toggle_descriptions();
        if (initializing_)
        {
            return;
        }
        update_preferences_.automatic_check_enabled = AutomaticUpdateCheckToggle().IsOn();
        update_preferences_.last_successful_check = 0;
        update_preferences_.retry_after = 0;
        update_preferences_.skipped_version.clear();
        update_update_frequency_dependency(true);
        glance::app::save_update_preferences(update_preferences_);
        if (update_preferences_changed_callback_)
        {
            update_preferences_changed_callback_();
        }
    }

    void SettingsWindow::UpdateCheckFrequencyComboBox_SelectionChanged(IInspectable const &,
                                                                       Controls::SelectionChangedEventArgs const &)
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

    void SettingsWindow::update_acrylic_dependency(bool animate)
    {
        settings_registry_.refresh(animate);
    }

    void SettingsWindow::update_update_frequency_dependency(bool animate)
    {
        settings_registry_.refresh(animate);
    }

    void SettingsWindow::save_appearance_preferences()
    {
        if (initializing_)
        {
            return;
        }
        const auto language_item = LanguageComboBox().SelectedItem().try_as<Controls::ComboBoxItem>();
        const auto language_tag =
            language_item == nullptr ? hstring(L"en-US") : unbox_value_or<hstring>(language_item.Tag(), L"en-US");
        appearance_preferences_.language = glance::app::resolve_ui_language(language_tag.c_str());
        appearance_preferences_.theme =
            static_cast<glance::app::ThemePreference>(std::clamp(ThemeComboBox().SelectedIndex(), 0, 2));
        appearance_preferences_.accent =
            static_cast<glance::app::AccentPreference>(std::clamp(AccentComboBox().SelectedIndex(), 0, 7));
        if (glance::app::acrylic_material_supported())
        {
            appearance_preferences_.acrylic_enabled = AcrylicMaterialToggle().IsOn();
        }
        appearance_preferences_.acrylic_opacity_percent =
            static_cast<std::uint32_t>(std::clamp(std::lround(AcrylicOpacitySlider().Value()), 10L, 100L));
        glance::app::save_appearance_preferences(appearance_preferences_);
        glance::app::apply_ui_language(appearance_preferences_.language);
        glance::app::apply_accent_resources(appearance_preferences_);
        ApplyLocalizedResources();
        if (appearance_changed_callback_)
        {
            appearance_changed_callback_();
        }
        else
        {
            ApplyAppearancePreferences();
        }
    }

    void SettingsWindow::AppearanceComboBox_SelectionChanged(IInspectable const &,
                                                             Controls::SelectionChangedEventArgs const &)
    {
        save_appearance_preferences();
    }

    void SettingsWindow::AcrylicMaterialToggle_Toggled(IInspectable const &, RoutedEventArgs const &)
    {
        update_acrylic_dependency(true);
        save_appearance_preferences();
    }

    void SettingsWindow::AcrylicOpacitySlider_ValueChanged(
        IInspectable const &, Controls::Primitives::RangeBaseValueChangedEventArgs const &args)
    {
        if (initializing_)
        {
            return;
        }

        appearance_preferences_.acrylic_opacity_percent =
            static_cast<std::uint32_t>(std::clamp(std::lround(args.NewValue()), 10L, 100L));
        glance::app::save_appearance_preferences(appearance_preferences_);
        if (appearance_changed_callback_)
        {
            appearance_changed_callback_();
        }
        else
        {
            ApplyAppearancePreferences();
        }
    }
} // namespace winrt::Glance::App::implementation

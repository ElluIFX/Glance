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
    void SettingsWindow::register_text_settings()
    {
        settings_registry_.register_page({L"text", L"", L"TextPreviewNavigationItem.Content",
                                          L"TextPreviewPageDescription.Text",
                                          glance::app::SettingsNavigationPosition::top});
        settings_registry_.register_section(
            {L"PlainTextPreviewSectionTitle", L"text", L"PlainTextPreviewSectionTitle.Text", {}});
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"FontFamilyLabel";
            definition.parent = L"PlainTextPreviewSectionTitle";
            definition.name_key = L"FontFamilyLabel.Text";
            definition.description_key = L"FontFamilyDescription.Text";
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<ComboBox xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" x:Name="FontFamilyComboBox" HorizontalAlignment="Stretch" VerticalAlignment="Center" Width="220" MinWidth="0" /> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"FontFamilyComboBox", control);
                const auto weak = get_weak();
                FontFamilyComboBox().SelectionChanged([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->FontFamilyComboBox_SelectionChanged(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"FontFamilyComboBox", [this] {
                settings_registry_.item(L"FontFamilyLabel");
                return settings_registry_.control(L"FontFamilyComboBox");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"FontSizeLabel";
            definition.parent = L"PlainTextPreviewSectionTitle";
            definition.name_key = L"FontSizeLabel.Text";
            definition.description_key = L"FontSizeDescription.Text";
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<NumberBox xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" x:Name="FontSizeNumberBox" HorizontalAlignment="Stretch" VerticalAlignment="Center" Maximum="32" Minimum="7" SmallChange="1" SpinButtonPlacementMode="Inline" /> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"FontSizeNumberBox", control);
                const auto weak = get_weak();
                FontSizeNumberBox().Loaded([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->NumberBox_Loaded(sender, args);
                });
                FontSizeNumberBox().ValueChanged([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->FontSizeNumberBox_ValueChanged(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"FontSizeNumberBox", [this] {
                settings_registry_.item(L"FontSizeLabel");
                return settings_registry_.control(L"FontSizeNumberBox");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"SyntaxHighlightingLabel";
            definition.parent = L"PlainTextPreviewSectionTitle";
            definition.name_key = L"SyntaxHighlightingLabel.Text";
            definition.description = [this] {
                return glance::app::SettingsText{SyntaxHighlightingToggle().IsOn()
                                                     ? L"SyntaxHighlightingEnabledDescription.Text"
                                                     : L"SyntaxHighlightingDisabledDescription.Text",
                                                 {}};
            };
            definition.create_control = [this] {
                auto control = glance::app::make_settings_toggle();
                settings_registry_.bind(L"SyntaxHighlightingToggle", control);
                const auto weak = get_weak();
                SyntaxHighlightingToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->TextPreferenceToggle_Toggled(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"SyntaxHighlightingToggle", [this] {
                settings_registry_.item(L"SyntaxHighlightingLabel");
                return settings_registry_.control(L"SyntaxHighlightingToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"SyntaxThemeLabel";
            definition.parent = L"PlainTextPreviewSectionTitle";
            definition.name_key = L"SyntaxThemeLabel.Text";
            definition.description_key = L"SyntaxThemeDescription.Text";
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<ComboBox xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" x:Name="SyntaxThemeComboBox" HorizontalAlignment="Stretch" VerticalAlignment="Center" Width="220" MinWidth="0" /> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"SyntaxThemeComboBox", control);
                settings_registry_.bind_choices(
                    L"SyntaxThemeComboBox",
                    {L"SyntaxThemeGlance", L"SyntaxThemeVisualStudio", L"SyntaxThemeMonokai", L"SyntaxThemeGitHub",
                     L"SyntaxThemeDracula", L"SyntaxThemeSolarized", L"SyntaxThemeNord", L"SyntaxThemeOneDark",
                     L"SyntaxThemeGruvbox", L"SyntaxThemeTomorrowNight", L"SyntaxThemeCatppuccin",
                     L"SyntaxThemeTokyoNight", L"SyntaxThemeRosePine", L"SyntaxThemeEverforest", L"SyntaxThemeAyu",
                     L"SyntaxThemeHorizon", L"SyntaxThemePaperColor", L"SyntaxThemeMaterial"});
                const auto weak = get_weak();
                SyntaxThemeComboBox().SelectionChanged([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->SyntaxThemeComboBox_SelectionChanged(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"SyntaxThemeComboBox", [this] {
                settings_registry_.item(L"SyntaxThemeLabel");
                return settings_registry_.control(L"SyntaxThemeComboBox");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"LineNumbersLabel";
            definition.parent = L"PlainTextPreviewSectionTitle";
            definition.name_key = L"LineNumbersLabel.Text";
            definition.description = [this] {
                return glance::app::SettingsText{LineNumbersToggle().IsOn() ? L"LineNumbersEnabledDescription.Text"
                                                                            : L"LineNumbersDisabledDescription.Text",
                                                 {}};
            };
            definition.create_control = [this] {
                auto control = glance::app::make_settings_toggle();
                settings_registry_.bind(L"LineNumbersToggle", control);
                const auto weak = get_weak();
                LineNumbersToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->TextPreferenceToggle_Toggled(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"LineNumbersToggle", [this] {
                settings_registry_.item(L"LineNumbersLabel");
                return settings_registry_.control(L"LineNumbersToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"WordWrapLabel";
            definition.parent = L"PlainTextPreviewSectionTitle";
            definition.name_key = L"WordWrapLabel.Text";
            definition.description = [this] {
                return glance::app::SettingsText{WordWrapToggle().IsOn() ? L"WordWrapEnabledDescription.Text"
                                                                         : L"WordWrapDisabledDescription.Text",
                                                 {}};
            };
            definition.create_control = [this] {
                auto control = glance::app::make_settings_toggle();
                settings_registry_.bind(L"WordWrapToggle", control);
                const auto weak = get_weak();
                WordWrapToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->TextPreferenceToggle_Toggled(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"WordWrapToggle", [this] {
                settings_registry_.item(L"WordWrapLabel");
                return settings_registry_.control(L"WordWrapToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
        settings_registry_.register_section(
            {L"MarkdownPreviewSectionTitle", L"text", L"MarkdownPreviewSectionTitle.Text", {}});
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"MarkdownDefaultPreviewLabel";
            definition.parent = L"MarkdownPreviewSectionTitle";
            definition.name_key = L"MarkdownDefaultPreviewLabel.Text";
            definition.description = [this] {
                return glance::app::SettingsText{MarkdownDefaultPreviewToggle().IsOn()
                                                     ? L"MarkdownDefaultPreviewEnabledDescription.Text"
                                                     : L"MarkdownDefaultPreviewDisabledDescription.Text",
                                                 {}};
            };
            definition.create_control = [this] {
                auto control = glance::app::make_settings_toggle();
                settings_registry_.bind(L"MarkdownDefaultPreviewToggle", control);
                const auto weak = get_weak();
                MarkdownDefaultPreviewToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->TextPreferenceToggle_Toggled(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"MarkdownDefaultPreviewToggle", [this] {
                settings_registry_.item(L"MarkdownDefaultPreviewLabel");
                return settings_registry_.control(L"MarkdownDefaultPreviewToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"MarkdownFontLabel";
            definition.parent = L"MarkdownPreviewSectionTitle";
            definition.name_key = L"MarkdownFontLabel.Text";
            definition.description_key = L"MarkdownFontDescription.Text";
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<ComboBox xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" x:Name="MarkdownFontComboBox" HorizontalAlignment="Stretch" VerticalAlignment="Center" Width="220" MinWidth="0" /> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"MarkdownFontComboBox", control);
                const auto weak = get_weak();
                MarkdownFontComboBox().SelectionChanged([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->FontFamilyComboBox_SelectionChanged(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"MarkdownFontComboBox", [this] {
                settings_registry_.item(L"MarkdownFontLabel");
                return settings_registry_.control(L"MarkdownFontComboBox");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"MarkdownSizeLabel";
            definition.parent = L"MarkdownPreviewSectionTitle";
            definition.name_key = L"MarkdownSizeLabel.Text";
            definition.description_key = L"MarkdownSizeDescription.Text";
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<NumberBox xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" x:Name="MarkdownSizeNumberBox" HorizontalAlignment="Stretch" VerticalAlignment="Center" Minimum="8" Maximum="48" SmallChange="1" SpinButtonPlacementMode="Inline" /> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"MarkdownSizeNumberBox", control);
                const auto weak = get_weak();
                MarkdownSizeNumberBox().Loaded([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->NumberBox_Loaded(sender, args);
                });
                MarkdownSizeNumberBox().ValueChanged([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->MarkdownSizeNumberBox_ValueChanged(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"MarkdownSizeNumberBox", [this] {
                settings_registry_.item(L"MarkdownSizeLabel");
                return settings_registry_.control(L"MarkdownSizeNumberBox");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"MarkdownStyleLabel";
            definition.parent = L"MarkdownPreviewSectionTitle";
            definition.name_key = L"MarkdownStyleLabel.Text";
            definition.description_key = L"MarkdownStyleDescription.Text";
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<ComboBox xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" x:Name="MarkdownStyleComboBox" HorizontalAlignment="Stretch" VerticalAlignment="Center" Width="220" MinWidth="0" /> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"MarkdownStyleComboBox", control);
                settings_registry_.bind_choices(L"MarkdownStyleComboBox",
                                                {L"MarkdownStyleAutomatic", L"MarkdownStyleGitHubLight",
                                                 L"MarkdownStyleGitHubDark", L"MarkdownStyleSolarizedLight",
                                                 L"MarkdownStyleSolarizedDark", L"MarkdownStyleNord",
                                                 L"MarkdownStyleDracula"});
                const auto weak = get_weak();
                MarkdownStyleComboBox().SelectionChanged([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->FontFamilyComboBox_SelectionChanged(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"MarkdownStyleComboBox", [this] {
                settings_registry_.item(L"MarkdownStyleLabel");
                return settings_registry_.control(L"MarkdownStyleComboBox");
            });
            settings_registry_.register_item(std::move(definition));
        }
        settings_registry_.register_section({L"JsonPreviewSectionTitle", L"text", L"JsonPreviewSectionTitle.Text", {}});
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"JsonDefaultTreeLabel";
            definition.parent = L"JsonPreviewSectionTitle";
            definition.name_key = L"JsonDefaultTreeLabel.Text";
            definition.description = [this] {
                return glance::app::SettingsText{JsonDefaultTreeToggle().IsOn()
                                                     ? L"JsonDefaultTreeEnabledDescription.Text"
                                                     : L"JsonDefaultTreeDisabledDescription.Text",
                                                 {}};
            };
            definition.create_control = [this] {
                auto control = glance::app::make_settings_toggle();
                settings_registry_.bind(L"JsonDefaultTreeToggle", control);
                const auto weak = get_weak();
                JsonDefaultTreeToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->TextPreferenceToggle_Toggled(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"JsonDefaultTreeToggle", [this] {
                settings_registry_.item(L"JsonDefaultTreeLabel");
                return settings_registry_.control(L"JsonDefaultTreeToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
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
        text_preferences_.markdown_default_preview = MarkdownDefaultPreviewToggle().IsOn();
        text_preferences_.json_default_tree = JsonDefaultTreeToggle().IsOn();
        if (MarkdownFontComboBox().SelectedItem())
            text_preferences_.markdown_font_family = unbox_value<hstring>(MarkdownFontComboBox().SelectedItem());
        if (std::isfinite(MarkdownSizeNumberBox().Value()))
            text_preferences_.markdown_font_size = MarkdownSizeNumberBox().Value();
        if (MarkdownStyleComboBox().SelectedIndex() >= 0)
            text_preferences_.markdown_style = static_cast<std::uint32_t>(MarkdownStyleComboBox().SelectedIndex());
        glance::app::save_text_preferences(text_preferences_);
        if (text_preferences_changed_callback_)
        {
            text_preferences_changed_callback_();
        }
    }

    void SettingsWindow::MarkdownSizeNumberBox_ValueChanged(IInspectable const &,
                                                            Controls::NumberBoxValueChangedEventArgs const &args)
    {
        if (initializing_)
            return;
        if (!std::isfinite(args.NewValue()))
        {
            initializing_ = true;
            MarkdownSizeNumberBox().Value(text_preferences_.markdown_font_size);
            initializing_ = false;
            return;
        }
        save_text_preferences();
    }

    void SettingsWindow::FontFamilyComboBox_SelectionChanged(IInspectable const &,
                                                             Controls::SelectionChangedEventArgs const &)
    {
        save_text_preferences();
    }

    void SettingsWindow::FontSizeNumberBox_ValueChanged(IInspectable const &,
                                                        Controls::NumberBoxValueChangedEventArgs const &args)
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

    void SettingsWindow::SyntaxThemeComboBox_SelectionChanged(IInspectable const &,
                                                              Controls::SelectionChangedEventArgs const &)
    {
        save_text_preferences();
    }

    void SettingsWindow::TextPreferenceToggle_Toggled(IInspectable const &, RoutedEventArgs const &)
    {
        refresh_toggle_descriptions();
        save_text_preferences();
    }
} // namespace winrt::Glance::App::implementation

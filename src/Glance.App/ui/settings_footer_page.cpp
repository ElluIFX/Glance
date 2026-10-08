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

namespace
{
    constexpr std::array footer_field_label_keys{L"FooterSizeLabel",
                                                 L"FooterModifiedTimeLabel",
                                                 L"FooterCreationTimeLabel",
                                                 L"FooterPermissionsLabel",
                                                 L"FooterMediaInfoLabel",
                                                 L"FooterTakenTimeLabel",
                                                 L"FooterCaptureParametersLabel",
                                                 L"FooterLineEndingsLabel"};

    Controls::Border make_footer_field_card(glance::app::FooterField field, bool enabled)
    {
        Controls::TextBlock label;
        label.Text(glance::app::localize(footer_field_label_keys[static_cast<std::size_t>(field)]));
        label.FontSize(14);
        label.VerticalAlignment(VerticalAlignment::Center);
        label.HorizontalAlignment(HorizontalAlignment::Center);
        Controls::Border card;
        card.Child(label);
        card.Tag(box_value(static_cast<std::uint32_t>(field)));
        card.IsHitTestVisible(false);
        card.Opacity(enabled ? 1.0 : 0.45);
        card.Height(34);
        card.Padding(Thickness{12, 0, 12, 0});
        card.CornerRadius(CornerRadius{4, 4, 4, 4});
        card.BorderThickness(Thickness{1, 1, 1, 1});
        const auto resources = Application::Current().Resources();
        card.Style(resources.Lookup(box_value(enabled ? L"SettingsFieldCardStyle" : L"SettingsDisabledFieldCardStyle"))
                       .as<Style>());
        return card;
    }
} // namespace

namespace winrt::Glance::App::implementation
{
    void SettingsWindow::register_footer_settings()
    {
        settings_registry_.register_page({L"footer", L"", L"FooterNavigationItem.Content",
                                          L"FooterPageDescription.Text", glance::app::SettingsNavigationPosition::top});
        settings_registry_.register_section({L"FooterFieldsLabel", L"footer", L"FooterFieldsLabel.Text", {}});
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"FooterFieldsLabel.custom22";
            definition.parent = L"FooterFieldsLabel";
            definition.create_control = [this] {
                auto control =
                    Markup::XamlReader::Load(
                        LR"XAML(<StackPanel xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" Padding="12,12,6,6" Spacing="4">
<GridView x:Name="FooterEnabledFields" SelectionMode="None" IsItemClickEnabled="True" Background="Transparent" IsTabStop="False" AllowFocusOnInteraction="False" MinHeight="0" Padding="0" CanDragItems="True" CanReorderItems="True" AllowDrop="True" ScrollViewer.HorizontalScrollMode="Disabled" ScrollViewer.HorizontalScrollBarVisibility="Disabled" ScrollViewer.VerticalScrollMode="Disabled" ScrollViewer.VerticalScrollBarVisibility="Disabled">
<GridView.ItemsPanel>
<ItemsPanelTemplate>
<ItemsWrapGrid Orientation="Horizontal" />
</ItemsPanelTemplate>
</GridView.ItemsPanel>
<GridView.ItemContainerStyle>
<Style TargetType="GridViewItem">
<Setter Property="IsTabStop" Value="False" />
<Setter Property="AllowFocusOnInteraction" Value="False" />
<Setter Property="Padding" Value="0" />
<Setter Property="Margin" Value="0,0,6,6" />
<Setter Property="MinWidth" Value="0" />
<Setter Property="MinHeight" Value="0" />
<Setter Property="HorizontalContentAlignment" Value="Stretch" />
</Style>
</GridView.ItemContainerStyle>
<GridView.ItemContainerTransitions>
<TransitionCollection>
<ReorderThemeTransition />
</TransitionCollection>
</GridView.ItemContainerTransitions>
</GridView>
<Line X2="1" Stretch="Fill" Height="1" Margin="0,0,6,4" Stroke="{ThemeResource DividerStrokeColorDefaultBrush}" StrokeDashArray="4,4" />
<GridView x:Name="FooterDisabledFields" SelectionMode="None" IsItemClickEnabled="True" Background="Transparent" IsTabStop="False" AllowFocusOnInteraction="False" MinHeight="0" Padding="0" CanDragItems="True" CanReorderItems="False" AllowDrop="True" ScrollViewer.HorizontalScrollMode="Disabled" ScrollViewer.HorizontalScrollBarVisibility="Disabled" ScrollViewer.VerticalScrollMode="Disabled" ScrollViewer.VerticalScrollBarVisibility="Disabled">
<GridView.ItemsPanel>
<ItemsPanelTemplate>
<ItemsWrapGrid Orientation="Horizontal" />
</ItemsPanelTemplate>
</GridView.ItemsPanel>
<GridView.ItemContainerStyle>
<Style TargetType="GridViewItem">
<Setter Property="IsTabStop" Value="False" />
<Setter Property="AllowFocusOnInteraction" Value="False" />
<Setter Property="Padding" Value="0" />
<Setter Property="Margin" Value="0,0,6,6" />
<Setter Property="MinWidth" Value="0" />
<Setter Property="MinHeight" Value="0" />
<Setter Property="HorizontalContentAlignment" Value="Stretch" />
</Style>
</GridView.ItemContainerStyle>
<GridView.ItemContainerTransitions>
<TransitionCollection>
<ReorderThemeTransition />
</TransitionCollection>
</GridView.ItemContainerTransitions>
</GridView>
</StackPanel> )XAML")
                        .as<FrameworkElement>();
                settings_registry_.bind(L"FooterEnabledFields",
                                        control.FindName(L"FooterEnabledFields").as<FrameworkElement>());
                settings_registry_.bind(L"FooterDisabledFields",
                                        control.FindName(L"FooterDisabledFields").as<FrameworkElement>());
                const auto weak = get_weak();
                FooterEnabledFields().DragOver([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->FooterFields_DragOver(sender, args);
                });
                FooterEnabledFields().ItemClick([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->FooterField_ItemClick(sender, args);
                });
                FooterEnabledFields().SizeChanged([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->FooterFields_SizeChanged(sender, args);
                });
                FooterEnabledFields().DragItemsCompleted([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->FooterFields_DragItemsCompleted(sender, args);
                });
                FooterEnabledFields().DragItemsStarting([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->FooterFields_DragItemsStarting(sender, args);
                });
                FooterEnabledFields().Drop([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->FooterFields_Drop(sender, args);
                });
                FooterEnabledFields().ContainerContentChanging([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->FooterFields_ContainerContentChanging(sender, args);
                });
                FooterDisabledFields().DragOver([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->FooterFields_DragOver(sender, args);
                });
                FooterDisabledFields().ItemClick([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->FooterField_ItemClick(sender, args);
                });
                FooterDisabledFields().SizeChanged([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->FooterFields_SizeChanged(sender, args);
                });
                FooterDisabledFields().DragItemsCompleted([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->FooterFields_DragItemsCompleted(sender, args);
                });
                FooterDisabledFields().DragItemsStarting([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->FooterFields_DragItemsStarting(sender, args);
                });
                FooterDisabledFields().Drop([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->FooterFields_Drop(sender, args);
                });
                FooterDisabledFields().ContainerContentChanging([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->FooterFields_ContainerContentChanging(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"FooterEnabledFields", [this] {
                settings_registry_.item(L"FooterFieldsLabel.custom22");
                return settings_registry_.control(L"FooterEnabledFields");
            });
            settings_registry_.bind_factory(L"FooterDisabledFields", [this] {
                settings_registry_.item(L"FooterFieldsLabel.custom22");
                return settings_registry_.control(L"FooterDisabledFields");
            });
            settings_registry_.register_custom_item(std::move(definition));
        }
        settings_registry_.register_section({L"PathCopyGroupLabel", L"footer", L"PathCopyGroupLabel.Text", {}});
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"QuoteCopiedPathLabel";
            definition.parent = L"PathCopyGroupLabel";
            definition.name_key = L"QuoteCopiedPathLabel.Text";
            definition.description = [this] {
                return glance::app::SettingsText{QuoteCopiedPathToggle().IsOn()
                                                     ? L"QuoteCopiedPathEnabledDescription.Text"
                                                     : L"QuoteCopiedPathDisabledDescription.Text",
                                                 {}};
            };
            definition.create_control = [this] {
                auto control = glance::app::make_settings_toggle();
                settings_registry_.bind(L"QuoteCopiedPathToggle", control);
                const auto weak = get_weak();
                QuoteCopiedPathToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->PathCopyPreferenceToggle_Toggled(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"QuoteCopiedPathToggle", [this] {
                settings_registry_.item(L"QuoteCopiedPathLabel");
                return settings_registry_.control(L"QuoteCopiedPathToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
        {
            glance::app::SettingsItemDefinition definition;
            definition.id = L"UnixPathSeparatorsLabel";
            definition.parent = L"PathCopyGroupLabel";
            definition.name_key = L"UnixPathSeparatorsLabel.Text";
            definition.description = [this] {
                return glance::app::SettingsText{UnixPathSeparatorsToggle().IsOn()
                                                     ? L"UnixPathSeparatorsEnabledDescription.Text"
                                                     : L"UnixPathSeparatorsDisabledDescription.Text",
                                                 {}};
            };
            definition.create_control = [this] {
                auto control = glance::app::make_settings_toggle();
                settings_registry_.bind(L"UnixPathSeparatorsToggle", control);
                const auto weak = get_weak();
                UnixPathSeparatorsToggle().Toggled([weak](auto const &sender, auto const &args) {
                    if (const auto self = weak.get())
                        self->PathCopyPreferenceToggle_Toggled(sender, args);
                });
                return control;
            };
            settings_registry_.bind_factory(L"UnixPathSeparatorsToggle", [this] {
                settings_registry_.item(L"UnixPathSeparatorsLabel");
                return settings_registry_.control(L"UnixPathSeparatorsToggle");
            });
            settings_registry_.register_item(std::move(definition));
        }
    }

    void SettingsWindow::PathCopyPreferenceToggle_Toggled(IInspectable const &, RoutedEventArgs const &)
    {
        refresh_toggle_descriptions();
        if (initializing_)
        {
            return;
        }
        path_copy_preferences_.quote_path = QuoteCopiedPathToggle().IsOn();
        path_copy_preferences_.use_unix_separators = UnixPathSeparatorsToggle().IsOn();
        glance::app::save_path_copy_preferences(path_copy_preferences_);
    }

    void SettingsWindow::rebuild_footer_field_cards()
    {
        FooterEnabledFields().Items().Clear();
        FooterDisabledFields().Items().Clear();
        std::array<Controls::Border, glance::app::footer_field_count> cards;
        double width = 0.0;
        for (const auto field : glance::app::FooterPreferences{}.order)
        {
            auto &card = cards[static_cast<std::size_t>(field)];
            card = make_footer_field_card(field, glance::app::footer_field_enabled(footer_preferences_, field));
            card.Measure(Windows::Foundation::Size{1000, 1000});
            width = std::max(width, static_cast<double>(card.DesiredSize().Width));
        }
        for (const auto field : footer_preferences_.order)
        {
            auto card = cards[static_cast<std::size_t>(field)];
            card.Width(std::ceil(width));
            if (glance::app::footer_field_enabled(footer_preferences_, field))
            {
                FooterEnabledFields().Items().Append(card);
            }
        }
        for (const auto field : glance::app::FooterPreferences{}.order)
        {
            if (!glance::app::footer_field_enabled(footer_preferences_, field))
            {
                FooterDisabledFields().Items().Append(cards[static_cast<std::size_t>(field)]);
            }
        }
        update_footer_field_heights();
    }

    void SettingsWindow::update_footer_field_heights()
    {
        for (const auto grid : {FooterEnabledFields(), FooterDisabledFields()})
        {
            const auto count = grid.Items().Size();
            double height = dragged_footer_card_ ? 34.0 : 0.0;
            if (count != 0)
            {
                const auto card = grid.Items().GetAt(0).as<Controls::Border>();
                const double cell_width = card.Width() + 6;
                const double cell_height = card.Height() + 6;
                const double columns = std::max(1.0, std::floor(grid.ActualWidth() / cell_width));
                height = std::ceil(count / columns) * cell_height;
                if (const auto panel = grid.ItemsPanelRoot().try_as<Controls::ItemsWrapGrid>())
                {
                    panel.ItemWidth(cell_width);
                    panel.ItemHeight(cell_height);
                }
            }
            grid.Height(height);
        }
    }

    void SettingsWindow::FooterFields_SizeChanged(IInspectable const &, SizeChangedEventArgs const &)
    {
        update_footer_field_heights();
    }

    void SettingsWindow::FooterFields_ContainerContentChanging(Controls::ListViewBase const &,
                                                               Controls::ContainerContentChangingEventArgs const &args)
    {
        if (args.InRecycleQueue() || !glance::app::settings_animations_enabled())
        {
            return;
        }
        const auto container = args.ItemContainer();
        const auto visual = Hosting::ElementCompositionPreview::GetElementVisual(container);
        const auto compositor = visual.Compositor();
        const auto movement = compositor.CreateVector3KeyFrameAnimation();
        movement.Target(L"Offset");
        movement.Duration(std::chrono::milliseconds(120));
        movement.InsertExpressionKeyFrame(1.0F, L"this.FinalValue",
                                          compositor.CreateCubicBezierEasingFunction({0.16F, 1.0F}, {0.30F, 1.0F}));
        const auto animations = compositor.CreateImplicitAnimationCollection();
        animations.Insert(L"Offset", movement);
        visual.ImplicitAnimations(animations);
        const auto appear = compositor.CreateScalarKeyFrameAnimation();
        appear.Target(L"Opacity");
        appear.Duration(std::chrono::milliseconds(100));
        appear.InsertKeyFrame(0.0F, 0.0F);
        appear.InsertKeyFrame(1.0F, 1.0F);
        Hosting::ElementCompositionPreview::SetImplicitShowAnimation(container, appear);
        const auto disappear = compositor.CreateScalarKeyFrameAnimation();
        disappear.Target(L"Opacity");
        disappear.Duration(std::chrono::milliseconds(80));
        disappear.InsertKeyFrame(0.0F, 1.0F);
        disappear.InsertKeyFrame(1.0F, 0.0F);
        Hosting::ElementCompositionPreview::SetImplicitHideAnimation(container, disappear);
    }

    void SettingsWindow::update_footer_field_order()
    {
        update_footer_field_heights();
        std::size_t index = 0;
        for (const auto item : FooterEnabledFields().Items())
        {
            footer_preferences_.order[index++] =
                static_cast<glance::app::FooterField>(unbox_value<std::uint32_t>(item.as<FrameworkElement>().Tag()));
        }
        for (const auto field : glance::app::FooterPreferences{}.order)
        {
            if (!glance::app::footer_field_enabled(footer_preferences_, field))
            {
                footer_preferences_.order[index++] = field;
            }
        }
        save_footer_preferences();
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

    void SettingsWindow::FooterField_ItemClick(IInspectable const &, Controls::ItemClickEventArgs const &args)
    {
        if (initializing_)
        {
            return;
        }
        toggle_footer_field(args.ClickedItem().as<Controls::Border>());
    }

    void SettingsWindow::toggle_footer_field(Controls::Border const &card)
    {
        const auto field = static_cast<glance::app::FooterField>(unbox_value<std::uint32_t>(card.Tag()));
        const bool enabled = !glance::app::footer_field_enabled(footer_preferences_, field);
        auto source = (enabled ? FooterDisabledFields() : FooterEnabledFields()).Items();
        std::uint32_t source_index{};
        if (!source.IndexOf(card, source_index))
        {
            return;
        }
        source.RemoveAt(source_index);
        footer_preferences_.enabled_mask ^= glance::app::footer_field_bit(field);
        // Keep the outgoing element available to its removal transition.
        const auto replacement = make_footer_field_card(field, enabled);
        replacement.Width(card.Width());
        if (enabled)
        {
            FooterEnabledFields().Items().Append(replacement);
        }
        else
        {
            std::uint32_t insertion_index = 0;
            for (const auto candidate : glance::app::FooterPreferences{}.order)
            {
                if (candidate == field)
                {
                    break;
                }
                if (!glance::app::footer_field_enabled(footer_preferences_, candidate))
                {
                    ++insertion_index;
                }
            }
            FooterDisabledFields().Items().InsertAt(insertion_index, replacement);
        }
        update_footer_field_order();
    }

    void SettingsWindow::FooterFields_DragItemsCompleted(Controls::ListViewBase const &,
                                                         Controls::DragItemsCompletedEventArgs const &)
    {
        dragged_footer_card_ = nullptr;
        update_footer_field_order();
    }

    void SettingsWindow::FooterFields_DragItemsStarting(IInspectable const &,
                                                        Controls::DragItemsStartingEventArgs const &args)
    {
        if (args.Items().Size() != 1)
        {
            args.Cancel(true);
            return;
        }
        dragged_footer_card_ = args.Items().GetAt(0).as<Controls::Border>();
        args.Data().RequestedOperation(Windows::ApplicationModel::DataTransfer::DataPackageOperation::Move);
        update_footer_field_heights();
    }

    void SettingsWindow::FooterFields_DragOver(IInspectable const &sender, DragEventArgs const &args)
    {
        using Windows::ApplicationModel::DataTransfer::DataPackageOperation;
        if (!dragged_footer_card_)
        {
            args.AcceptedOperation(DataPackageOperation::None);
            args.Handled(true);
            return;
        }
        const auto target = sender.as<Controls::GridView>();
        std::uint32_t index{};
        const bool same_region = target.Items().IndexOf(dragged_footer_card_, index);
        if (same_region && target == FooterEnabledFields())
        {
            return;
        }
        args.AcceptedOperation(same_region ? DataPackageOperation::None : DataPackageOperation::Move);
        args.Handled(true);
    }

    void SettingsWindow::FooterFields_Drop(IInspectable const &sender, DragEventArgs const &args)
    {
        if (!dragged_footer_card_)
        {
            return;
        }
        const auto target = sender.as<Controls::GridView>();
        std::uint32_t index{};
        if (target.Items().IndexOf(dragged_footer_card_, index))
        {
            return;
        }
        toggle_footer_field(dragged_footer_card_);
        args.AcceptedOperation(Windows::ApplicationModel::DataTransfer::DataPackageOperation::Move);
        args.Handled(true);
    }
} // namespace winrt::Glance::App::implementation

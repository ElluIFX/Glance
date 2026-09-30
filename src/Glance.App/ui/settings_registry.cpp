#include "pch.h"
#include "settings_registry.h"
#include "glance/contracts/diagnostics.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.ViewManagement.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;

namespace glance::app
{
    bool settings_animations_enabled() noexcept
    {
        BOOL enabled = TRUE;
        return !SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) || enabled != FALSE;
    }
    namespace
    {
        void animate_opacity(FrameworkElement const& element, float from, float to)
        {
            const auto visual = Hosting::ElementCompositionPreview::GetElementVisual(element);
            auto animation = visual.Compositor().CreateScalarKeyFrameAnimation();
            animation.InsertKeyFrame(0, from);
            animation.InsertKeyFrame(1, to);
            animation.Duration(std::chrono::milliseconds(100));
            visual.StartAnimation(L"Opacity", animation);
        }
        void set_enabled(FrameworkElement const &element, bool enabled)
        {
            if (const auto control = element.try_as<Control>())
                control.IsEnabled(enabled);
            else if (const auto panel = element.try_as<Panel>())
                for (const auto &child : panel.Children())
                    if (const auto item = child.try_as<FrameworkElement>())
                        set_enabled(item, enabled);
        }
        TextBlock label(bool secondary = false)
        {
            TextBlock text;
            text.TextWrapping(TextWrapping::Wrap);
            if (secondary)
            {
                text.FontSize(12);
                text.Style(
                    Application::Current().Resources().Lookup(box_value(L"SettingsDescriptionStyle")).as<Style>());
                text.Visibility(Visibility::Collapsed);
                text.RegisterPropertyChangedCallback(TextBlock::TextProperty(), [](DependencyObject const &sender,
                                                                                   auto const &) {
                    const auto description = sender.as<TextBlock>();
                    description.Visibility(description.Text().empty() ? Visibility::Collapsed : Visibility::Visible);
                });
            }
            return text;
        }

        void animate_position(Panel const &panel)
        {
            if (!Windows::UI::ViewManagement::UISettings().AnimationsEnabled())
                return;
            panel.LayoutUpdated([weak = make_weak(panel),
                                 positions = std::make_shared<std::map<std::uint64_t, float>>()](auto const &,
                                                                                                 auto const &) {
                const auto parent = weak.get();
                if (!parent)
                    return;
                for (const auto &child : parent.Children())
                {
                    const auto element = child.try_as<FrameworkElement>();
                    if (!element || element.Visibility() == Visibility::Collapsed)
                        continue;
                    const auto key = reinterpret_cast<std::uint64_t>(get_abi(element));
                    const auto position = element.ActualOffset().y;
                    const auto found = positions->find(key);
                    if (found != positions->end() && std::abs(found->second - position) > 0.5f)
                    {
                        const auto visual = Hosting::ElementCompositionPreview::GetElementVisual(element);
                        auto animation = visual.Compositor().CreateVector3KeyFrameAnimation();
                        animation.InsertKeyFrame(0,
                                                 Windows::Foundation::Numerics::float3{0, found->second - position, 0});
                        animation.InsertKeyFrame(1, Windows::Foundation::Numerics::float3{});
                        animation.Duration(std::chrono::milliseconds(110));
                        Hosting::ElementCompositionPreview::SetIsTranslationEnabled(element, true);
                        visual.StartAnimation(L"Translation", animation);
                    }
                    positions->insert_or_assign(key, position);
                }
            });
        }
    } // namespace

    ToggleSwitch make_settings_toggle()
    {
        ToggleSwitch control;
        control.Style(Application::Current().Resources().Lookup(box_value(L"SettingsToggleStyle")).as<Style>());
        return control;
    }

    ComboBox make_settings_choice()
    {
        ComboBox control;
        control.Width(220);
        return control;
    }

    NumberBox make_settings_number(double minimum, double maximum, double step)
    {
        NumberBox control;
        control.Width(132);
        control.Minimum(minimum);
        control.Maximum(maximum);
        control.SmallChange(step);
        control.SpinButtonPlacementMode(NumberBoxSpinButtonPlacementMode::Inline);
        return control;
    }

    Slider make_settings_slider(double minimum, double maximum, double step)
    {
        Slider control;
        control.Width(220);
        control.Minimum(minimum);
        control.Maximum(maximum);
        control.StepFrequency(step);
        return control;
    }

    TextBox make_settings_text()
    {
        TextBox control;
        control.Width(220);
        return control;
    }

    Button make_settings_button()
    {
        Button control;
        control.IsTabStop(false);
        control.AllowFocusOnInteraction(false);
        return control;
    }

    SettingsRowView make_settings_row(FrameworkElement const &operation, std::wstring_view glyph)
    {
        SettingsRowView result{Grid(), label(), label(true), StackPanel()};
        auto row = result.root;
        row.MinHeight(56);
        row.Padding(Thickness{16, 10, 16, 10});
        row.ColumnSpacing(16);
        ColumnDefinition text_column, control_column;
        text_column.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        control_column.Width(GridLengthHelper::Auto());
        row.ColumnDefinitions().Append(text_column);
        row.ColumnDefinitions().Append(control_column);
        RowDefinition first, second;
        first.Height(GridLengthHelper::Auto());
        second.Height(GridLengthHelper::Auto());
        row.RowDefinitions().Append(first);
        row.RowDefinitions().Append(second);
        result.text.Spacing(3);
        result.text.VerticalAlignment(VerticalAlignment::Center);
        Grid heading;
        ColumnDefinition icon_column, title_column;
        icon_column.Width(GridLengthHelper::Auto());
        heading.ColumnDefinitions().Append(icon_column);
        heading.ColumnDefinitions().Append(title_column);
        if (!glyph.empty())
        {
            FontIcon icon;
            icon.FontSize(16);
            icon.Glyph(glyph);
            icon.Margin(Thickness{0, 0, 8, 0});
            heading.Children().Append(icon);
        }
        Grid::SetColumn(result.title, 1);
        heading.Children().Append(result.title);
        result.text.Children().Append(heading);
        result.text.Children().Append(result.description);
        row.Children().Append(result.text);
        operation.VerticalAlignment(VerticalAlignment::Center);
        operation.HorizontalAlignment(HorizontalAlignment::Right);
        Grid::SetColumn(operation, 1);
        row.Children().Append(operation);
        row.SizeChanged([control_column, operation, text = result.text](auto const &sender, auto const &) {
            const bool narrow = sender.template as<Grid>().ActualWidth() <
                                std::max(320.0, static_cast<double>(operation.DesiredSize().Width) + 248.0);
            Grid::SetRow(operation, narrow ? 1 : 0);
            Grid::SetColumn(operation, narrow ? 0 : 1);
            Grid::SetColumnSpan(text, narrow ? 2 : 1);
            Grid::SetColumnSpan(operation, narrow ? 2 : 1);
            operation.Margin(narrow ? Thickness{0, 8, 0, 0} : Thickness{});
            operation.HorizontalAlignment(narrow ? HorizontalAlignment::Left : HorizontalAlignment::Right);
            control_column.Width(narrow ? GridLengthHelper::FromPixels(0) : GridLengthHelper::Auto());
        });
        return result;
    }

    struct SettingsRegistry::Node
    {
        enum class Kind
        {
            page,
            section,
            item,
            custom
        } kind{};
        std::wstring id, parent, name_key, description_key, icon;
        SettingsNavigationPosition position{};
        SettingsItemDefinition definition;
        std::vector<std::wstring> children;
        FrameworkElement element{nullptr};
        StackPanel children_panel{nullptr};
        TextBlock title{nullptr}, description{nullptr};
        NavigationViewItem navigation{nullptr};
        Border card{nullptr};
        Shapes::Rectangle divider{nullptr};
        ProgressRing progress{nullptr};
        FrameworkElement control{nullptr};
        DispatcherTimer hide_timer{nullptr};
        bool visible{true};
        bool creation_failed{};
    };

    SettingsRegistry::SettingsRegistry(Resolver resolver) : resolve_(std::move(resolver))
    {
    }
    SettingsRegistry::~SettingsRegistry()
    {
        for (auto &[id, value] : nodes_)
        {
            static_cast<void>(id);
            if (value->hide_timer)
                value->hide_timer.Stop();
        }
        if (host_)
            host_.Children().Clear();
        controls_.clear();
    }

    SettingsRegistry::Node &SettingsRegistry::node(std::wstring_view id)
    {
        const auto found = nodes_.find(id);
        if (found == nodes_.end())
            throw std::invalid_argument("Unknown settings parent or item");
        return *found->second;
    }

    void SettingsRegistry::register_page(SettingsPageDefinition definition)
    {
        if (definition.id.empty() || nodes_.contains(definition.id))
            throw std::invalid_argument("Duplicate or empty settings page ID");
        auto value = std::make_unique<Node>();
        value->kind = Node::Kind::page;
        value->id = definition.id;
        value->name_key = std::move(definition.name_key);
        value->description_key = std::move(definition.description_key);
        value->icon = std::move(definition.icon);
        value->position = definition.position;
        value->definition.resource_owner = std::move(definition.resource_owner);
        pages_.push_back(definition.id);
        nodes_.emplace(definition.id, std::move(value));
    }

    void SettingsRegistry::register_section(SettingsSectionDefinition definition)
    {
        auto &parent = node(definition.page);
        if (parent.kind != Node::Kind::page || definition.id.empty() || nodes_.contains(definition.id))
            throw std::invalid_argument("Invalid settings section");
        auto value = std::make_unique<Node>();
        value->kind = Node::Kind::section;
        value->id = definition.id;
        value->parent = definition.page;
        value->name_key = std::move(definition.name_key);
        value->description_key = std::move(definition.description_key);
        parent.children.push_back(definition.id);
        value->definition.resource_owner = std::move(definition.resource_owner);
        nodes_.emplace(definition.id, std::move(value));
    }

    void SettingsRegistry::add_item(SettingsItemDefinition definition, bool custom)
    {
        auto &parent = node(definition.parent);
        if ((parent.kind != Node::Kind::page && parent.kind != Node::Kind::section) || definition.id.empty() ||
            nodes_.contains(definition.id) || !definition.create_control)
            throw std::invalid_argument("Invalid settings item");
        auto value = std::make_unique<Node>();
        value->kind = custom ? Node::Kind::custom : Node::Kind::item;
        value->id = definition.id;
        value->parent = definition.parent;
        value->name_key = definition.name_key;
        value->description_key = definition.description_key;
        value->icon = definition.icon;
        value->definition = std::move(definition);
        parent.children.push_back(value->id);
        const auto id = value->id;
        nodes_.emplace(id, std::move(value));
    }

    void SettingsRegistry::register_item(SettingsItemDefinition definition)
    {
        add_item(std::move(definition), false);
    }
    void SettingsRegistry::register_custom_item(SettingsItemDefinition definition)
    {
        add_item(std::move(definition), true);
    }

    void SettingsRegistry::initialize(NavigationView const &navigation, Grid const &host)
    {
        host_ = host;
        for (const auto &id : pages_)
        {
            auto &value = node(id);
            NavigationViewItem entry;
            entry.Tag(box_value(id));
            entry.Content(box_value(resolve_(value.definition.resource_owner, value.name_key)));
            if (!value.icon.empty())
            {
                FontIcon icon;
                icon.Glyph(value.icon);
                entry.Icon(icon);
            }
            value.navigation = entry;
            auto entries = value.position == SettingsNavigationPosition::top ? navigation.MenuItems()
                                                                             : navigation.FooterMenuItems();
            entries.InsertAt(value.position == SettingsNavigationPosition::top ? entries.Size()
                             : entries.Size() >= 2                             ? entries.Size() - 2
                                                                               : 0,
                             entry);
        }
        if (!pages_.empty())
            navigation.SelectedItem(node(pages_.front()).navigation);
    }

    SettingsRegistry::Element SettingsRegistry::build(Node &value)
    {
        if (value.element)
            return value.element;
        if (value.kind == Node::Kind::page || value.kind == Node::Kind::section)
        {
            StackPanel panel;
            panel.Spacing(value.kind == Node::Kind::page ? 20 : 8);
            animate_position(panel);
            value.title = label();
            value.title.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            if (value.kind == Node::Kind::page)
                value.title.FontSize(24);
            StackPanel heading;
            heading.Spacing(4);
            heading.Children().Append(value.title);
            value.description = label(true);
            heading.Children().Append(value.description);
            panel.Children().Append(heading);
            value.children_panel = panel;
            if (value.kind == Node::Kind::page)
            {
                panel.MaxWidth(720);
                panel.HorizontalAlignment(HorizontalAlignment::Stretch);
                panel.Margin(Thickness{0, 0, 16, 0});
                ScrollViewer scroller;
                scroller.HorizontalContentAlignment(HorizontalAlignment::Stretch);
                scroller.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
                scroller.HorizontalScrollMode(ScrollMode::Disabled);
                scroller.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
                scroller.Content(panel);
                value.element = scroller;
            }
            else
                value.element = panel;
            StackPanel rows{nullptr};
            Border group_card{nullptr};
            for (const auto &child_id : value.children)
            {
                auto &child = node(child_id);
                if (child.kind == Node::Kind::section)
                {
                    rows = nullptr;
                    panel.Children().Append(build(child));
                    continue;
                }
                if (!rows || child.kind == Node::Kind::custom)
                {
                    Border card;
                    card.Style(Application::Current().Resources().Lookup(box_value(L"SettingsGroupStyle")).as<Style>());
                    rows = StackPanel();
                    animate_position(rows);
                    card.Child(rows);
                    panel.Children().Append(card);
                    group_card = card;
                }
                child.card = group_card;
                rows.Children().Append(build(child));
                if (child.kind == Node::Kind::custom)
                    rows = nullptr;
            }
        }
        else
        {
            StackPanel wrapper;
            value.element = wrapper;
            value.divider = Shapes::Rectangle();
            value.divider.Style(
                Application::Current().Resources().Lookup(box_value(L"SettingsDividerStyle")).as<Style>());
            wrapper.Children().Append(value.divider);
            try
            {
                value.control = value.definition.create_control();
                if (!value.control)
                    throw std::runtime_error("Settings factory returned no control");
            }
            catch (...)
            {
                value.creation_failed = true;
                value.visible = false;
                wrapper.Visibility(Visibility::Collapsed);
                glance::contracts::log_event(L"Settings control creation failed: " + value.id);
                return wrapper;
            }
            if (const auto toggle = value.control.try_as<ToggleSwitch>())
                toggle.Style(Application::Current().Resources().Lookup(box_value(L"SettingsToggleStyle")).as<Style>());
            if (value.kind == Node::Kind::custom)
            {
                wrapper.Children().Append(value.control);
            }
            else
            {
                StackPanel operation;
                operation.Orientation(Orientation::Horizontal);
                operation.Spacing(8);
                operation.VerticalAlignment(VerticalAlignment::Center);
                operation.HorizontalAlignment(HorizontalAlignment::Right);
                value.progress = ProgressRing();
                value.progress.Width(20);
                value.progress.Height(20);
                value.progress.Visibility(Visibility::Collapsed);
                operation.Children().Append(value.progress);
                operation.Children().Append(value.control);
                const auto row = make_settings_row(operation, value.icon);
                row.root.SizeChanged([control = value.control](auto const &sender, auto const &) {
                    const auto width = sender.template as<FrameworkElement>().ActualWidth();
                    if (width > 0)
                        control.MaxWidth(std::max(132.0, width < 480 ? width - 32 : width * 0.6));
                });
                value.title = row.title;
                value.description = row.description;
                wrapper.Children().Append(row.root);
            }
        }
        refresh_node(value, false);
        return value.element;
    }

    bool SettingsRegistry::page_created(std::wstring_view id) const
    {
        const auto found = nodes_.find(id);
        return found != nodes_.end() && found->second->kind == Node::Kind::page && found->second->element;
    }

    void SettingsRegistry::show_page(std::wstring_view id)
    {
        if (selected_ == id)
            return;
        for (auto const &child : host_.Children())
        {
            Hosting::ElementCompositionPreview::SetIsTranslationEnabled(child, true);
            const auto visual = Hosting::ElementCompositionPreview::GetElementVisual(child);
            visual.StopAnimation(L"Opacity");
            visual.StopAnimation(L"Translation");
            visual.Opacity(0);
            child.IsHitTestVisible(false);
            child.Visibility(Visibility::Collapsed);
        }
        for (auto &[page_id, value] : nodes_)
        {
            if (value->kind != Node::Kind::page)
                continue;
            value->visible = page_id == id;
            if (value->hide_timer)
                value->hide_timer.Stop();
        }
        const auto found = nodes_.find(id);
        selected_ = id;
        if (found == nodes_.end())
            return;
        auto element = build(*found->second);
        if (!element.Parent())
            host_.Children().Append(element);
        element.Visibility(Visibility::Visible);
        element.IsHitTestVisible(true);
        element.Opacity(1);
        const auto visual = Hosting::ElementCompositionPreview::GetElementVisual(element);
        visual.Opacity(1);
        if (Windows::UI::ViewManagement::UISettings().AnimationsEnabled())
        {
            auto animation = visual.Compositor().CreateScalarKeyFrameAnimation();
            animation.InsertKeyFrame(0, 0.0f);
            animation.InsertKeyFrame(1, 1.0f);
            animation.Duration(std::chrono::milliseconds(120));
            visual.StartAnimation(L"Opacity", animation);
            Hosting::ElementCompositionPreview::SetIsTranslationEnabled(element, true);
            auto translation = visual.Compositor().CreateVector3KeyFrameAnimation();
            translation.InsertKeyFrame(0, Windows::Foundation::Numerics::float3{0, 6, 0});
            translation.InsertKeyFrame(1, Windows::Foundation::Numerics::float3{});
            translation.Duration(std::chrono::milliseconds(120));
            visual.StartAnimation(L"Translation", translation);
        }
    }

    void SettingsRegistry::refresh_node(Node &value, bool animate)
    {
        if (!value.element)
            return;
        if (value.title)
            value.title.Text(resolve_(value.definition.resource_owner, value.name_key));
        if (value.description && (value.definition.description || !value.description_key.empty()))
        {
            const auto text =
                value.definition.description ? value.definition.description() : SettingsText{value.description_key, {}};
            std::wstring resolved =
                text.key.empty() ? L"" : std::wstring(resolve_(value.definition.resource_owner, text.key));
            for (std::size_t index = 0; index < text.arguments.size(); ++index)
            {
                const auto token = L"{" + std::to_wstring(index) + L"}";
                std::size_t offset{};
                while ((offset = resolved.find(token, offset)) != std::wstring::npos)
                {
                    resolved.replace(offset, token.size(), text.arguments[index]);
                    offset += text.arguments[index].size();
                }
            }
            value.description.Text(resolved);
            value.description.Visibility(resolved.empty() ? Visibility::Collapsed : Visibility::Visible);
        }
        const bool was_visible = value.visible;
        const bool visible = value.kind == Node::Kind::page
                                 ? selected_ == value.id
                                 : !value.creation_failed && (!value.definition.visible || value.definition.visible());
        value.visible = visible;
        value.element.IsHitTestVisible(visible);
        if (value.kind == Node::Kind::page)
        {
            value.element.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
        }
        else if (visible != was_visible || !animate)
        {
            if (value.hide_timer)
                value.hide_timer.Stop();
            if (visible || !animate || !Windows::UI::ViewManagement::UISettings().AnimationsEnabled())
            {
                value.element.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
                value.element.Opacity(visible ? 1 : 0);
                if (visible && !was_visible && animate && settings_animations_enabled())
                    animate_opacity(value.element, 0, 1);
            }
            else
            {
                if (!value.hide_timer)
                {
                    value.hide_timer = DispatcherTimer();
                    value.hide_timer.Interval(std::chrono::milliseconds(100));
                    value.hide_timer.Tick([this, id = value.id](auto const &, auto const &) {
                        auto &current = node(id);
                        current.hide_timer.Stop();
                        if (!current.visible)
                            current.element.Visibility(Visibility::Collapsed);
                        update_dividers(node(current.parent));
                    });
                }
                animate_opacity(value.element, 1, 0);
                value.hide_timer.Start();
            }
        }
        if (value.control)
        {
            const bool busy = value.definition.busy && value.definition.busy();
            if (value.definition.enabled || value.definition.busy)
            {
                const bool enabled = !busy && (!value.definition.enabled || value.definition.enabled());
                value.control.IsHitTestVisible(enabled);
                set_enabled(value.control, enabled);
            }
            if (value.progress)
            {
                value.progress.IsActive(busy);
                value.progress.Visibility(busy ? Visibility::Visible : Visibility::Collapsed);
            }
        }
        if (value.definition.refresh)
            value.definition.refresh();
        update_dividers(value);
    }

    void SettingsRegistry::update_dividers(Node &value)
    {
        bool previous{};
        std::vector<std::pair<Border, bool>> cards;
        for (const auto &id : value.children)
        {
            auto &child = node(id);
            if (child.kind == Node::Kind::section || child.kind == Node::Kind::custom)
                previous = false;
            if (child.divider)
                child.divider.Visibility(previous && child.visible ? Visibility::Visible : Visibility::Collapsed);
            if (child.visible)
                previous = child.kind == Node::Kind::item;
            if (child.card)
            {
                auto found = std::ranges::find_if(cards, [&](const auto &card) { return card.first == child.card; });
                if (found == cards.end())
                    cards.emplace_back(child.card, child.visible);
                else
                    found->second = found->second || child.visible;
            }
        }
        for (const auto &[card, visible] : cards)
            card.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
        if (value.kind == Node::Kind::section && value.element)
        {
            value.visible = std::ranges::any_of(value.children, [this](const auto &id) { return node(id).visible; });
            value.element.Visibility(value.visible ? Visibility::Visible : Visibility::Collapsed);
        }
    }

    void SettingsRegistry::refresh(bool animate)
    {
        for (auto &[id, value] : nodes_)
        {
            static_cast<void>(id);
            refresh_node(*value, animate);
        }
        for (auto &[id, value] : nodes_)
        {
            static_cast<void>(id);
            update_dividers(*value);
        }
    }

    void SettingsRegistry::refresh_item(std::wstring_view id, bool animate)
    {
        auto &value = node(id);
        refresh_node(value, animate);
        if (!value.parent.empty())
            update_dividers(node(value.parent));
    }

    void SettingsRegistry::bind_choices(std::wstring name, std::vector<std::wstring> keys)
    {
        const auto combo = control(name).as<ComboBox>();
        for (const auto &key : keys)
        {
            ComboBoxItem item;
            item.Content(box_value(resolve_({}, key)));
            combo.Items().Append(item);
        }
        choices_.emplace(std::move(name), std::move(keys));
    }

    void SettingsRegistry::localize()
    {
        for (auto &[id, value] : nodes_)
        {
            static_cast<void>(id);
            if (value->navigation)
                value->navigation.Content(box_value(resolve_(value->definition.resource_owner, value->name_key)));
        }
        for (const auto &binding : texts_)
        {
            const auto found = controls_.find(binding.name);
            if (found == controls_.end())
                continue;
            if (binding.content)
                found->second.as<ContentControl>().Content(box_value(resolve_({}, binding.key)));
            else
                found->second.as<TextBlock>().Text(resolve_({}, binding.key));
        }
        for (const auto &[name, keys] : choices_)
        {
            const auto combo = control(name).as<ComboBox>();
            for (std::uint32_t index = 0; index < keys.size(); ++index)
                combo.Items().GetAt(index).as<ComboBoxItem>().Content(box_value(resolve_({}, keys[index])));
        }
        for (const auto &[name, element] : controls_)
        {
            static_cast<void>(name);
            if (const auto combo = element.try_as<ComboBox>(); combo && !combo.IsDropDownOpen())
            {
                const auto index = combo.SelectedIndex();
                combo.SelectedIndex(-1);
                combo.SelectedIndex(index);
            }
        }
        refresh();
    }

    void SettingsRegistry::bind(std::wstring name, Element const &element)
    {
        if (element.Name().empty())
            element.Name(name);
        if (auto toggle = element.try_as<ToggleSwitch>())
            toggle.Style(Application::Current().Resources().Lookup(box_value(L"SettingsToggleStyle")).as<Style>());
        controls_.insert_or_assign(std::move(name), element);
    }
    void SettingsRegistry::bind_factory(std::wstring name, std::function<Element()> factory)
    {
        factories_.insert_or_assign(std::move(name), std::move(factory));
    }
    SettingsRegistry::Element SettingsRegistry::control(std::wstring_view name)
    {
        if (const auto found = controls_.find(name); found != controls_.end())
            return found->second;
        if (const auto found = factories_.find(name); found != factories_.end())
        {
            auto factory = std::move(found->second);
            factories_.erase(found);
            return factory();
        }
        throw std::invalid_argument("Unknown settings control");
    }
    void SettingsRegistry::bind_text(std::wstring name, std::wstring key, bool content)
    {
        texts_.push_back({std::move(name), std::move(key), content});
    }
    SettingsRegistry::Element SettingsRegistry::item(std::wstring_view id)
    {
        return build(node(id));
    }
    SettingsRegistry::Element SettingsRegistry::item_title(std::wstring_view id)
    {
        build(node(id));
        return node(id).title;
    }
    SettingsRegistry::Element SettingsRegistry::item_description(std::wstring_view id)
    {
        build(node(id));
        return node(id).description;
    }
} // namespace glance::app

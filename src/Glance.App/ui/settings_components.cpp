#include "pch.h"
#include "SettingsWindow.xaml.h"
#include "component_loader.h"
#include "localization.h"
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <cmath>
#include <set>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace glance::contracts::components;

namespace
{
    bool contains_focus(FrameworkElement const& element)
    {
        if (!element.XamlRoot()) return false;
        auto focused = Microsoft::UI::Xaml::Input::FocusManager::GetFocusedElement(element.XamlRoot()).try_as<DependencyObject>();
        while (focused)
        {
            if (focused == element) return true;
            focused = Media::VisualTreeHelper::GetParent(focused);
        }
        return false;
    }
    struct SettingControl
    {
        ComponentSettingDescriptor descriptor;
        FrameworkElement control{nullptr};
        bool updating{};
    };

    SettingsItemState read_state(ComponentSettingDescriptor const &descriptor)
    {
        SettingsItemState state;
        if (descriptor.query_state && !descriptor.query_state(descriptor.setting_id, &state))
            return {};
        state.description_key[setting_text_capacity - 1] = 0;
        state.argument_count = std::min(state.argument_count, 4U);
        for (auto &argument : state.arguments)
            argument[setting_text_capacity - 1] = 0;
        return state;
    }

    struct CustomSettingsView
    {
        std::shared_ptr<void> lease;
        std::wstring component;
        SettingsCustomItemDescriptor descriptor;
        SettingsViewHost host;
        std::function<void()> changed;
        FrameworkElement element{nullptr};
        std::uint64_t session{};

        ~CustomSettingsView()
        {
            element = nullptr;
            if (session)
                descriptor.close(session);
        }
    };
} // namespace

namespace winrt::Glance::App::implementation
{
    void SettingsWindow::register_component_settings()
    {
        const auto weak = get_weak();
        for (const auto &registration : glance::app::component_settings_registrations())
        {
            const auto owner = registration.component_id;
            std::set<std::wstring> owned;
            const auto qualify = [&](const wchar_t *id) {
                return owned.contains(id) ? owner + L"/" + id : std::wstring(id);
            };
            for (std::size_t index = 0; index < registration.entries.size(); ++index)
            {
                const auto &entry = registration.entries[index];
                if (const auto page = std::get_if<SettingsPageDescriptor>(&entry))
                {
                    owned.insert(page->id);
                    settings_registry_.register_page({owner + L"/" + page->id, page->icon, page->name_key,
                                                      page->description_key,
                                                      page->bottom ? glance::app::SettingsNavigationPosition::bottom
                                                                   : glance::app::SettingsNavigationPosition::top,
                                                      owner});
                }
                else if (const auto section = std::get_if<SettingsSectionDescriptor>(&entry))
                {
                    const auto parent = qualify(section->page);
                    owned.insert(section->id);
                    settings_registry_.register_section(
                        {owner + L"/" + section->id, parent, section->name_key, section->description_key, owner});
                }
                else if (const auto custom = std::get_if<SettingsCustomItemDescriptor>(&entry))
                {
                    auto state = std::make_shared<CustomSettingsView>();
                    state->lease = registration.lease;
                    state->component = owner;
                    state->descriptor = *custom;
                    const auto id = owner + L"/" + custom->id;
                    state->changed = [weak, id, owner] {
                        if (auto self = weak.get())
                        {
                            self->DispatcherQueue().TryEnqueue([weak, id, owner] {
                                if (auto window = weak.get())
                                {
                                    window->settings_registry_.refresh_item(id);
                                    if (window->component_setting_changed_callback_)
                                        window->component_setting_changed_callback_(owner);
                                }
                            });
                        }
                    };
                    state->host.context = state.get();
                    state->host.read_value = [](void *context, const wchar_t *key, std::int64_t fallback) noexcept {
                        return key ? glance::app::component_setting_value(
                                         static_cast<CustomSettingsView *>(context)->component, key, fallback)
                                   : fallback;
                    };
                    state->host.write_value = [](void *context, const wchar_t *key,
                                                 std::int64_t value) noexcept -> BOOL {
                        if (!key)
                            return FALSE;
                        glance::app::save_component_setting_value(static_cast<CustomSettingsView *>(context)->component,
                                                                  key, value);
                        return glance::app::component_setting_value(static_cast<CustomSettingsView *>(context)->component,
                            key, ~value) == value;
                    };
                    state->host.refresh = [](void *context) noexcept {
                        try
                        {
                            static_cast<CustomSettingsView *>(context)->changed();
                        }
                        catch (...)
                        {
                        }
                    };
                    glance::app::SettingsItemDefinition item;
                    item.id = id;
                    item.parent = qualify(custom->parent);
                    item.resource_owner = owner;
                    item.create_control = [state] {
                        com_ptr<::IUnknown> element;
                        check_hresult(state->descriptor.create(glance::app::current_ui_language().c_str(), &state->host,
                                                               element.put(), &state->session));
                        state->element = element.as<FrameworkElement>();
                        return state->element;
                    };
                    item.refresh = [state] {
                        if (state->session)
                            state->descriptor.refresh(state->session, glance::app::current_ui_language().c_str());
                    };
                    settings_registry_.register_custom_item(std::move(item));
                }
                else if (const auto descriptor = std::get_if<ComponentSettingDescriptor>(&entry))
                {
                    std::vector<std::shared_ptr<SettingControl>> fields;
                    fields.push_back(std::make_shared<SettingControl>(*descriptor));
                    if (descriptor->row_id[0])
                    {
                        while (index + 1 < registration.entries.size())
                        {
                            const auto next = std::get_if<ComponentSettingDescriptor>(&registration.entries[index + 1]);
                            if (!next || std::wstring_view(next->row_id) != descriptor->row_id ||
                                std::wstring_view(next->parent) != descriptor->parent)
                                break;
                            fields.push_back(std::make_shared<SettingControl>(*next));
                            ++index;
                        }
                    }
                    const auto id = owner + L"/" + descriptor->setting_id;
                    glance::app::SettingsItemDefinition item;
                    item.id = id;
                    item.parent = qualify(descriptor->parent);
                    item.name_key = descriptor->row_id[0] ? descriptor->row_title_key : descriptor->label_key;
                    item.description_key = descriptor->description_key;
                    item.resource_owner = owner;
                    item.icon = descriptor->icon;
                    if (descriptor->query_state)
                    {
                        item.visible = [value = *descriptor] { return read_state(value).visible != FALSE; };
                        item.enabled = [value = *descriptor] { return read_state(value).enabled != FALSE; };
                        item.busy = [value = *descriptor] { return read_state(value).busy != FALSE; };
                    }
                    item.description = [owner, value = *descriptor] {
                        const auto state = read_state(value);
                        if (state.description_key[0])
                        {
                            glance::app::SettingsText text{state.description_key};
                            for (std::uint32_t index = 0; index < state.argument_count; ++index)
                                text.arguments.emplace_back(state.arguments[index]);
                            return text;
                        }
                        if (value.kind != ComponentSettingKind::toggle)
                            return glance::app::SettingsText{value.description_key};
                        const bool enabled =
                            glance::app::component_setting_value(owner, value.setting_id, value.default_value) != 0;
                        const auto key = enabled ? value.enabled_description_key : value.disabled_description_key;
                        return glance::app::SettingsText{key[0] ? key : value.description_key};
                    };
                    item.create_control = [weak, owner, id, fields, lease = registration.lease]() -> FrameworkElement {
                        static_cast<void>(lease);
                        Grid panel;
                        panel.ColumnSpacing(12);
                        panel.RowSpacing(8);
                        for (const auto &field : fields)
                        {
                            const auto value = field->descriptor;
                            const auto save = [weak, owner, id, field_ref = std::weak_ptr(field)](std::int64_t number) {
                                const auto field = field_ref.lock();
                                if (!field || field->updating)
                                    return;
                                if (auto self = weak.get(); self && !self->initializing_)
                                {
                                    glance::app::save_component_setting_value(owner, field->descriptor.setting_id,
                                                                              number);
                                    self->settings_registry_.refresh_item(id);
                                    if (self->component_setting_changed_callback_)
                                        self->component_setting_changed_callback_(owner);
                                }
                            };
                            if (value.kind == ComponentSettingKind::toggle)
                            {
                                auto toggle = glance::app::make_settings_toggle();
                                toggle.Toggled([save](IInspectable const &sender, RoutedEventArgs const &) {
                                    save(sender.as<ToggleSwitch>().IsOn() ? 1 : 0);
                                });
                                field->control = toggle;
                            }
                            else if (value.kind == ComponentSettingKind::choice)
                            {
                                auto combo = glance::app::make_settings_choice();
                                for (std::uint32_t option = 0; option < value.option_count; ++option)
                                {
                                    ComboBoxItem choice;
                                    combo.Items().Append(choice);
                                }
                                combo.SelectionChanged(
                                    [save, value](IInspectable const &sender, SelectionChangedEventArgs const &) {
                                        const auto selected = sender.as<ComboBox>().SelectedIndex();
                                        if (selected >= 0 && selected < static_cast<int>(value.option_count))
                                            save(value.options[selected].value);
                                    });
                                field->control = combo;
                            }
                            else
                            {
                                const auto scale = std::pow(10.0, value.decimal_places);
                                auto number =
                                    glance::app::make_settings_number(static_cast<double>(value.minimum_value) / scale,
                                                                      static_cast<double>(value.maximum_value) / scale,
                                                                      static_cast<double>(value.small_change) / scale);
                                Windows::Globalization::NumberFormatting::DecimalFormatter formatter;
                                formatter.FractionDigits(value.decimal_places);
                                formatter.IsGrouped(false);
                                number.NumberFormatter(formatter);
                                number.Loaded([weak](IInspectable const &sender, RoutedEventArgs const &args) {
                                    if (const auto self = weak.get())
                                        self->NumberBox_Loaded(sender, args);
                                });
                                number.ValueChanged([save, value, scale](IInspectable const &,
                                                                         NumberBoxValueChangedEventArgs const &args) {
                                    if (std::isfinite(args.NewValue()))
                                        save(
                                            std::clamp(static_cast<std::int64_t>(std::llround(args.NewValue() * scale)),
                                                       value.minimum_value, value.maximum_value));
                                });
                                field->control = number;
                            }
                            panel.Children().Append(field->control);
                        }
                        panel.Width(fields.size() == 1 ? std::numeric_limits<double>::quiet_NaN()
                                                       : fields.size() * 144.0 - 12.0);
                        panel.SizeChanged([weak_panel = make_weak(panel), count = fields.size()](auto const &,
                                                                                                 auto const &) {
                            const auto panel = weak_panel.get();
                            if (!panel || count < 2)
                                return;
                            const auto columns =
                                std::max(1U, std::min(static_cast<unsigned>(count),
                                                      static_cast<unsigned>((panel.ActualWidth() + 12) / 144)));
                            if (panel.ColumnDefinitions().Size() == columns)
                                return;
                            panel.ColumnDefinitions().Clear();
                            panel.RowDefinitions().Clear();
                            for (unsigned column = 0; column < columns; ++column)
                                panel.ColumnDefinitions().Append(ColumnDefinition());
                            for (std::size_t row = 0; row < (count + columns - 1) / columns; ++row)
                                panel.RowDefinitions().Append(RowDefinition());
                            for (std::uint32_t child = 0; child < panel.Children().Size(); ++child)
                            {
                                Grid::SetColumn(panel.Children().GetAt(child).as<FrameworkElement>(), child % columns);
                                Grid::SetRow(panel.Children().GetAt(child).as<FrameworkElement>(), child / columns);
                            }
                        });
                        return panel;
                    };
                    item.refresh = [owner, fields] {
                        for (const auto &field : fields)
                        {
                            if (!field->control)
                                continue;
                            field->updating = true;
                            const auto &value = field->descriptor;
                            const auto stored =
                                glance::app::component_setting_value(owner, value.setting_id, value.default_value);
                            if (auto toggle = field->control.try_as<ToggleSwitch>())
                                toggle.IsOn(stored != 0);
                            else if (auto number = field->control.try_as<NumberBox>())
                            {
                                number.Header(value.row_id[0]
                                                  ? box_value(glance::app::localize_component(owner, value.label_key))
                                                  : nullptr);
                                if (!contains_focus(number))
                                    number.Value(static_cast<double>(
                                                     std::clamp(stored, value.minimum_value, value.maximum_value)) /
                                                 std::pow(10.0, value.decimal_places));
                            }
                            else if (auto combo = field->control.try_as<ComboBox>())
                            {
                                int selected = -1;
                                int fallback = 0;
                                for (std::uint32_t option = 0; option < value.option_count; ++option)
                                {
                                    combo.Items().GetAt(option).as<ComboBoxItem>().Content(box_value(
                                        glance::app::localize_component(owner, value.options[option].text_key)));
                                    if (value.options[option].value == stored)
                                        selected = static_cast<int>(option);
                                    if (value.options[option].value == value.default_value)
                                        fallback = static_cast<int>(option);
                                }
                                combo.SelectedIndex(selected < 0 ? fallback : selected);
                            }
                            field->updating = false;
                        }
                    };
                    settings_registry_.register_item(std::move(item));
                }
            }
        }
    }
} // namespace winrt::Glance::App::implementation

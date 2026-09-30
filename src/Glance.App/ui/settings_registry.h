#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

namespace glance::app
{
    bool settings_animations_enabled() noexcept;
    struct SettingsRowView
    {
        winrt::Microsoft::UI::Xaml::Controls::Grid root{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::TextBlock title{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::TextBlock description{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::StackPanel text{nullptr};
    };

    SettingsRowView make_settings_row(winrt::Microsoft::UI::Xaml::FrameworkElement const &operation,
                                      std::wstring_view icon = {});
    winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch make_settings_toggle();
    winrt::Microsoft::UI::Xaml::Controls::ComboBox make_settings_choice();
    winrt::Microsoft::UI::Xaml::Controls::NumberBox make_settings_number(double minimum, double maximum,
                                                                         double step = 1);
    winrt::Microsoft::UI::Xaml::Controls::Slider make_settings_slider(double minimum, double maximum, double step = 1);
    winrt::Microsoft::UI::Xaml::Controls::TextBox make_settings_text();
    winrt::Microsoft::UI::Xaml::Controls::Button make_settings_button();

    struct SettingsText
    {
        std::wstring key;
        std::vector<std::wstring> arguments;
    };

    enum class SettingsNavigationPosition
    {
        top,
        bottom
    };

    struct SettingsPageDefinition
    {
        std::wstring id;
        std::wstring icon;
        std::wstring name_key;
        std::wstring description_key;
        SettingsNavigationPosition position{SettingsNavigationPosition::top};
        std::wstring resource_owner;
    };

    struct SettingsSectionDefinition
    {
        std::wstring id;
        std::wstring page;
        std::wstring name_key;
        std::wstring description_key;
        std::wstring resource_owner;
    };

    struct SettingsItemDefinition
    {
        std::wstring id;
        std::wstring parent;
        std::wstring name_key;
        std::wstring description_key;
        std::wstring icon;
        std::function<winrt::Microsoft::UI::Xaml::FrameworkElement()> create_control;
        std::function<SettingsText()> description;
        std::function<bool()> visible;
        std::function<bool()> enabled;
        std::function<bool()> busy;
        std::function<void()> refresh;
        std::wstring resource_owner;
    };

    class SettingsRegistry
    {
      public:
        using Element = winrt::Microsoft::UI::Xaml::FrameworkElement;
        using Resolver = std::function<winrt::hstring(std::wstring_view, std::wstring_view)>;
        explicit SettingsRegistry(Resolver resolver);
        ~SettingsRegistry();
        void register_page(SettingsPageDefinition definition);
        void register_section(SettingsSectionDefinition definition);
        void register_item(SettingsItemDefinition definition);
        void register_custom_item(SettingsItemDefinition definition);
        void initialize(winrt::Microsoft::UI::Xaml::Controls::NavigationView const &navigation,
                        winrt::Microsoft::UI::Xaml::Controls::Grid const &host);
        void show_page(std::wstring_view id);
        bool page_created(std::wstring_view id) const;
        void refresh(bool animate = false);
        void refresh_item(std::wstring_view id, bool animate = false);
        void localize();
        void bind(std::wstring name, Element const &element);
        void bind_factory(std::wstring name, std::function<Element()> factory);
        Element control(std::wstring_view name);
        void bind_text(std::wstring name, std::wstring key, bool content = false);
        void bind_choices(std::wstring name, std::vector<std::wstring> keys);
        Element item(std::wstring_view id);
        Element item_title(std::wstring_view id);
        Element item_description(std::wstring_view id);

      private:
        struct Node;
        std::map<std::wstring, std::unique_ptr<Node>, std::less<>> nodes_;
        std::vector<std::wstring> pages_;
        std::map<std::wstring, Element, std::less<>> controls_;
        std::map<std::wstring, std::function<Element()>, std::less<>> factories_;
        struct TextBinding
        {
            std::wstring name;
            std::wstring key;
            bool content{};
        };
        std::vector<TextBinding> texts_;
        std::map<std::wstring, std::vector<std::wstring>, std::less<>> choices_;
        Resolver resolve_;
        winrt::Microsoft::UI::Xaml::Controls::Grid host_{nullptr};
        std::wstring selected_;
        Node &node(std::wstring_view id);
        void add_item(SettingsItemDefinition definition, bool custom);
        Element build(Node &value);
        void refresh_node(Node &value, bool animate);
        void update_dividers(Node &value);
    };
} // namespace glance::app

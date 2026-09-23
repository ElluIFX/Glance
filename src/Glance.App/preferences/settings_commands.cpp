#include "pch.h"
#include "settings_commands.h"
#include "public_settings.h"
#include "component_loader.h"
#include "startup_registration.h"
#include "footer_preferences.h"
#include "text_preferences.h"
#include "window_preferences.h"
#include "localization.h"
#include "glance/contracts/cli_protocol.h"
#include "glance/contracts/diagnostics.h"
#include <charconv>
#include <set>

using namespace winrt;
using namespace Windows::Data::Json;
using glance::cli::Error;

namespace
{
    std::vector<std::wstring> split(std::wstring text)
    {
        std::vector<std::wstring> result;
        std::size_t start = 0;
        for (;;)
        {
            const auto end = text.find(L'|', start);
            result.push_back(text.substr(start, end == text.npos ? end : end - start));
            if (end == text.npos) return result;
            start = end + 1;
        }
    }
    std::pair<std::wstring, std::wstring> registry_location(std::wstring key)
    {
        const auto divider = key.rfind(L'/');
        auto name = key.substr(divider + 1);
        key.resize(divider);
        std::replace(key.begin(), key.end(), L'/', L'\\');
        return { L"Software\\Glance\\" + key, name };
    }
    std::int64_t integer(std::wstring_view text)
    {
        const auto input = to_string(text);
        std::int64_t result{};
        const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), result);
        if (error != std::errc{} || end != input.data() + input.size()) throw Error(2, "invalid_value", "Expected an integer");
        return result;
    }
    IJsonValue value_from_text(const glance::app::PublicSetting& definition, std::wstring text)
    {
        if (text.find(L'\0') != text.npos) throw Error(2, "invalid_value", "Embedded null is not permitted");
        if (definition.type == L"string")
        {
            if (text.size() > static_cast<std::size_t>(definition.maximum)) throw Error(2, "invalid_value", "String exceeds setting limit");
            if (!definition.choices.empty())
            {
                const auto choices = split(definition.choices);
                if (std::find(choices.begin(), choices.end(), text) == choices.end()) throw Error(2, "invalid_value", "Value is not a supported choice");
            }
            return JsonValue::CreateStringValue(text);
        }
        if (definition.type == L"array")
        {
            auto array = JsonArray::Parse(text);
            if (array.Size() != glance::app::footer_field_count) throw Error(2, "invalid_value", "Expected a permutation of footer field IDs");
            std::set<int> entries;
            for (auto item : array)
            {
                const auto number = item.GetNumber();
                if (number < 0 || number >= glance::app::footer_field_count || std::floor(number) != number || !entries.insert(static_cast<int>(number)).second)
                    throw Error(2, "invalid_value", "Expected a permutation of footer field IDs");
            }
            return array;
        }
        if (definition.type == L"boolean")
        {
            if (text == L"true" || text == L"on") text = L"1";
            if (text == L"false" || text == L"off") text = L"0";
        }
        const auto number = integer(text);
        if (number < definition.minimum || number > definition.maximum) throw Error(2, "invalid_value", "Value is outside the setting range");
        if (!definition.choices.empty())
        {
            const auto choices = split(definition.choices);
            if (std::find(choices.begin(), choices.end(), std::to_wstring(number)) == choices.end()) throw Error(2, "invalid_value", "Value is not a supported choice");
        }
        return definition.type == L"boolean" ? JsonValue::CreateBooleanValue(number != 0) : JsonValue::CreateNumberValue(static_cast<double>(number));
    }
    IJsonValue read_value(const glance::app::PublicSetting& definition)
    {
        if (definition.key == L"Window/AutoFitMedia") return JsonValue::CreateBooleanValue(glance::app::load_window_preferences().auto_fit_media);
        if (definition.key == L"Startup/LaunchAtSignIn") return JsonValue::CreateBooleanValue(glance::app::launch_at_sign_in_enabled());
        if (definition.key == L"Diagnostics/Enabled") return JsonValue::CreateBooleanValue(glance::contracts::diagnostics_enabled());
        if (definition.key.starts_with(L"Components/"))
        {
            const auto part = definition.key.find(L'/', 11);
            const auto value = glance::app::component_setting_value(definition.key.substr(11, part - 11), definition.key.substr(part + 1), integer(definition.default_value));
            return value_from_text(definition, std::to_wstring(value));
        }
        if (definition.key == L"Footer/FieldOrder")
        {
            JsonArray result;
            for (const auto field : glance::app::load_footer_preferences().order) result.Append(JsonValue::CreateNumberValue(static_cast<unsigned>(field)));
            return result;
        }
        auto [path, name] = registry_location(definition.key);
        if (definition.type == L"string")
        {
            DWORD bytes{};
            std::wstring text = definition.default_value;
            if (RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name.c_str(), RRF_RT_REG_SZ, nullptr, nullptr, &bytes) == ERROR_SUCCESS && bytes <= 65536)
            {
                std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1);
                if (RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name.c_str(), RRF_RT_REG_SZ, nullptr, buffer.data(), &bytes) == ERROR_SUCCESS) text = buffer.data();
            }
            return JsonValue::CreateStringValue(text);
        }
        return value_from_text(definition, std::to_wstring(glance::app::read_public_dword(path.c_str(), name.c_str(), static_cast<DWORD>(integer(definition.default_value)))));
    }
    void write_value(const glance::app::PublicSetting& definition, IJsonValue const& value, bool reset)
    {
        if (definition.key == L"Startup/LaunchAtSignIn")
        {
            if (!glance::app::set_launch_at_sign_in(value.GetBoolean())) throw Error(7, "setting_write_failed", "Cannot change startup registration");
            return;
        }
        if (definition.key == L"Diagnostics/Enabled")
        {
            glance::contracts::set_diagnostics_enabled(value.GetBoolean());
            if (glance::contracts::diagnostics_enabled() != value.GetBoolean()) throw Error(7, "setting_write_failed", "Cannot change diagnostics setting");
            return;
        }
        if (definition.key.starts_with(L"Components/"))
        {
            const auto part = definition.key.find(L'/', 11);
            auto component = definition.key.substr(11, part - 11), key = definition.key.substr(part + 1);
            const auto number = definition.type == L"boolean" ? (value.GetBoolean() ? 1LL : 0LL) : static_cast<std::int64_t>(value.GetNumber());
            glance::app::save_component_setting_value(component, key, number);
            if (glance::app::component_setting_value(component, key, number - 1) != number) throw Error(7, "setting_write_failed", "Cannot save component setting");
            return;
        }
        auto [path, name] = registry_location(definition.key);
        HKEY key{};
        if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
            throw Error(7, "setting_write_failed", "Cannot open settings registry key");
        struct Close { HKEY key; ~Close() { RegCloseKey(key); } } close{ key };
        LSTATUS status{};
        // An explicit default must override the legacy root-level auto-fit value.
        if (reset && definition.key != L"Window/AutoFitMedia") status = RegDeleteValueW(key, name.c_str());
        else if (definition.type == L"string")
        {
            auto text = value.GetString();
            status = RegSetValueExW(key, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(text.c_str()), static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)));
        }
        else if (definition.type == L"array")
        {
            std::vector<DWORD> values;
            for (auto item : value.GetArray()) values.push_back(static_cast<DWORD>(item.GetNumber()));
            status = RegSetValueExW(key, name.c_str(), 0, REG_BINARY, reinterpret_cast<const BYTE*>(values.data()), static_cast<DWORD>(values.size() * sizeof(DWORD)));
        }
        else
        {
            DWORD number = definition.type == L"boolean" ? value.GetBoolean() : static_cast<DWORD>(value.GetNumber());
            status = RegSetValueExW(key, name.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&number), sizeof(number));
        }
        if (status != ERROR_SUCCESS && !(reset && status == ERROR_FILE_NOT_FOUND)) throw Error(7, "setting_write_failed", "Cannot save setting");
    }
}

namespace glance::app
{
    JsonObject execute_settings_command(std::wstring_view command, JsonObject const& request)
    {
        auto definitions = public_setting_definitions();
        for (const auto& setting : component_settings(current_ui_language()))
        {
            PublicSetting definition{ L"Components/" + setting.component_id + L"/" + setting.setting_id,
                setting.kind == glance::contracts::components::ComponentSettingKind::toggle ? L"boolean" : L"integer",
                std::to_wstring(setting.default_value), setting.minimum_value, setting.maximum_value, L"", L"immediate" };
            if (definition.type == L"boolean") { definition.minimum = 0; definition.maximum = 1; }
            if (!setting.options.empty())
            {
                definition.minimum = definition.maximum = setting.options.front().value;
                for (const auto& option : setting.options)
                {
                    definition.minimum = (std::min)(definition.minimum, option.value);
                    definition.maximum = (std::max)(definition.maximum, option.value);
                    if (!definition.choices.empty()) definition.choices += L"|";
                    definition.choices += std::to_wstring(option.value);
                }
            }
            definitions.push_back(std::move(definition));
        }
        std::sort(definitions.begin(), definitions.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
        const std::wstring key(request.GetNamedString(L"key", L""));
        if (command != L"settings.list")
        {
            const auto found = std::find_if(definitions.begin(), definitions.end(), [&](const auto& definition) { return definition.key == key; });
            if (found == definitions.end()) throw Error(3, "setting_not_found", "Public setting not found");
            if (command == L"settings.set" || command == L"settings.reset")
            {
                const bool reset = command == L"settings.reset";
                auto value = value_from_text(*found, reset ? found->default_value : std::wstring(request.GetNamedString(L"value")));
                if (key == L"Window/AdaptiveMinimumPercent" || key == L"Window/AdaptiveMaximumPercent")
                {
                    const bool minimum = key == L"Window/AdaptiveMinimumPercent";
                    const auto other = read_public_dword(L"Software\\Glance\\Window", minimum ? L"AdaptiveMaximumPercent" : L"AdaptiveMinimumPercent", minimum ? 75 : 40);
                    if ((minimum && value.GetNumber() > other) || (!minimum && value.GetNumber() < other)) throw Error(2, "invalid_range", "Adaptive minimum must not exceed maximum");
                }
                write_value(*found, value, reset);
            }
            else if (command != L"settings.get") throw Error(2, "unknown_command", "Unknown settings command");
        }
        JsonArray settings;
        for (const auto& definition : definitions)
        {
            if (command == L"settings.list" ? !definition.key.starts_with(key) : definition.key != key) continue;
            JsonObject item;
            item.SetNamedValue(L"key", JsonValue::CreateStringValue(definition.key));
            item.SetNamedValue(L"type", JsonValue::CreateStringValue(definition.type));
            item.SetNamedValue(L"value", read_value(definition));
            item.SetNamedValue(L"default", value_from_text(definition, definition.default_value));
            item.SetNamedValue(L"minimum", JsonValue::CreateNumberValue(static_cast<double>(definition.minimum)));
            item.SetNamedValue(L"maximum", JsonValue::CreateNumberValue(static_cast<double>(definition.maximum)));
            item.SetNamedValue(L"effect", JsonValue::CreateStringValue(definition.effect));
            JsonArray choices;
            if (!definition.choices.empty()) for (const auto& option : split(definition.choices)) choices.Append(JsonValue::CreateStringValue(option));
            item.SetNamedValue(L"choices", choices);
            settings.Append(item);
        }
        JsonObject result;
        result.SetNamedValue(L"settings", settings);
        return result;
    }
}

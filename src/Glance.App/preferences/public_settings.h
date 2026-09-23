#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace glance::app
{
    // Public settings are declared by their owning preference modules, not by CLI.
    struct PublicSetting
    {
        std::wstring key;
        std::wstring type;
        std::wstring default_value;
        std::int64_t minimum{};
        std::int64_t maximum{};
        std::wstring choices;
        std::wstring effect{ L"immediate" };
    };
    inline std::vector<PublicSetting>& public_setting_definitions()
    {
        static std::vector<PublicSetting> definitions;
        return definitions;
    }
    struct RegisterPublicSettings
    {
        RegisterPublicSettings(std::initializer_list<PublicSetting> values)
        {
            auto& definitions = public_setting_definitions();
            definitions.insert(definitions.end(), values.begin(), values.end());
        }
    };
    inline DWORD read_public_dword(const wchar_t* registry_path, const wchar_t* name, DWORD fallback) noexcept
    {
        try
        {
            std::wstring key(registry_path);
            constexpr std::wstring_view prefix = L"Software\\Glance\\";
            if (key.starts_with(prefix)) key.erase(0, prefix.size());
            std::replace(key.begin(), key.end(), L'\\', L'/');
            key += L"/"; key += name;
            const auto& definitions = public_setting_definitions();
            const auto definition = std::find_if(definitions.begin(), definitions.end(), [&](const auto& item) { return item.key == key; });
            DWORD value = fallback, bytes = sizeof(value);
            if (RegGetValueW(HKEY_CURRENT_USER, registry_path, name, RRF_RT_REG_DWORD, nullptr, &value, &bytes) != ERROR_SUCCESS)
                value = fallback; // Preserve module-specific migration and environment defaults.
            if (definition != definitions.end())
                value = static_cast<DWORD>(std::clamp<std::int64_t>(value, definition->minimum, definition->maximum));
            return value;
        }
        catch (...) { return fallback; }
    }
}

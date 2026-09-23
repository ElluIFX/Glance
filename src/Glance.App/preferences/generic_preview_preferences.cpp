#include "pch.h"
#include "generic_preview_preferences.h"
#include "public_settings.h"

namespace
{
    const glance::app::RegisterPublicSettings public_settings{
        { L"GenericPreview/ShowAdvancedInfo", L"boolean", L"0", 0, 1, L"", L"next_preview" },
    };
    constexpr wchar_t registry_path[] = L"Software\\Glance\\GenericPreview";

    DWORD read_dword(const wchar_t* name, DWORD fallback) noexcept
    {
        return glance::app::read_public_dword(registry_path, name, fallback);
    }
}

namespace glance::app
{
    GenericPreviewPreferences load_generic_preview_preferences() noexcept
    {
        return {
            .show_advanced_info = read_dword(L"ShowAdvancedInfo", 0) != 0,
        };
    }

    void save_generic_preview_preferences(const GenericPreviewPreferences& preferences) noexcept
    {
        HKEY key{};
        if (RegCreateKeyExW(
                HKEY_CURRENT_USER,
                registry_path,
                0,
                nullptr,
                0,
                KEY_SET_VALUE,
                nullptr,
                &key,
                nullptr) != ERROR_SUCCESS)
        {
            return;
        }

        const DWORD show_advanced_info = preferences.show_advanced_info;
        RegSetValueExW(
            key,
            L"ShowAdvancedInfo",
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&show_advanced_info),
            sizeof(show_advanced_info));
        RegCloseKey(key);
    }
}

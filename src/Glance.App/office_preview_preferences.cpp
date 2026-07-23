#include "pch.h"
#include "office_preview_preferences.h"

#include <algorithm>

namespace
{
    constexpr wchar_t registry_path[] = L"Software\\Glance\\OfficePreview";

    DWORD read_dword(const wchar_t* name, DWORD fallback) noexcept
    {
        DWORD value{};
        DWORD size = sizeof(value);
        return RegGetValueW(
                   HKEY_CURRENT_USER,
                   registry_path,
                   name,
                   RRF_RT_REG_DWORD,
                   nullptr,
                   &value,
                   &size) == ERROR_SUCCESS
            ? value
            : fallback;
    }

    void write_dword(HKEY key, const wchar_t* name, DWORD value) noexcept
    {
        RegSetValueExW(
            key,
            name,
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&value),
            sizeof(value));
    }
}

namespace glance::app
{
    OfficePreviewPreferences load_office_preview_preferences() noexcept
    {
        return {
            .cache_capacity = std::min<DWORD>(read_dword(L"CacheCapacity", 1), 16),
            .cache_expiration_minutes = std::clamp<DWORD>(
                read_dword(L"CacheExpirationMinutes", 5),
                1,
                60),
        };
    }

    void save_office_preview_preferences(const OfficePreviewPreferences& preferences) noexcept
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

        write_dword(
            key,
            L"CacheCapacity",
            std::min<std::uint32_t>(preferences.cache_capacity, 16));
        write_dword(
            key,
            L"CacheExpirationMinutes",
            std::clamp<std::uint32_t>(preferences.cache_expiration_minutes, 1, 60));
        RegCloseKey(key);
    }
}

#include "pch.h"
#include "window_preferences.h"

#include <algorithm>

namespace
{
    constexpr wchar_t registry_path[] = L"Software\\Glance\\Window";

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

    bool read_legacy_auto_fit() noexcept
    {
        DWORD value{};
        DWORD size = sizeof(value);
        return RegGetValueW(
                   HKEY_CURRENT_USER,
                   L"Software\\Glance",
                   L"AutoFitWindowSize",
                   RRF_RT_REG_DWORD,
                   nullptr,
                   &value,
                   &size) == ERROR_SUCCESS
            ? value != 0
            : true;
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
    WindowPreferences load_window_preferences() noexcept
    {
        return {
            .default_width = std::clamp<DWORD>(read_dword(L"DefaultWidth", 720), 480, 7680),
            .default_height = std::clamp<DWORD>(read_dword(L"DefaultHeight", 520), 320, 4320),
            .remember_size = read_dword(L"RememberSize", 1) != 0,
            .auto_fit_media = read_dword(L"AutoFitMedia", read_legacy_auto_fit() ? 1U : 0U) != 0,
            .remember_position = read_dword(L"RememberPosition", 0) != 0,
            .opacity_percent = std::clamp<DWORD>(read_dword(L"OpacityPercent", 100), 10, 100),
        };
    }

    void save_window_preferences(const WindowPreferences& preferences) noexcept
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

        write_dword(key, L"DefaultWidth", std::clamp<std::uint32_t>(preferences.default_width, 480, 7680));
        write_dword(key, L"DefaultHeight", std::clamp<std::uint32_t>(preferences.default_height, 320, 4320));
        write_dword(key, L"RememberSize", preferences.remember_size ? 1U : 0U);
        write_dword(key, L"AutoFitMedia", preferences.auto_fit_media ? 1U : 0U);
        write_dword(key, L"RememberPosition", preferences.remember_position ? 1U : 0U);
        write_dword(key, L"OpacityPercent", std::clamp<std::uint32_t>(preferences.opacity_percent, 10, 100));
        RegCloseKey(key);
    }
}

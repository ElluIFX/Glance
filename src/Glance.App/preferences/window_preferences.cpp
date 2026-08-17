#include "pch.h"
#include "window_preferences.h"

#include <algorithm>
#include <cwctype>
#include <vector>

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

    std::wstring read_string(const wchar_t* name) noexcept
    {
        DWORD size{};
        if (RegGetValueW(
                HKEY_CURRENT_USER,
                registry_path,
                name,
                RRF_RT_REG_SZ,
                nullptr,
                nullptr,
                &size) != ERROR_SUCCESS || size < sizeof(wchar_t))
        {
            return {};
        }

        std::vector<wchar_t> buffer(size / sizeof(wchar_t));
        if (RegGetValueW(
                HKEY_CURRENT_USER,
                registry_path,
                name,
                RRF_RT_REG_SZ,
                nullptr,
                buffer.data(),
                &size) != ERROR_SUCCESS)
        {
            return {};
        }
        return buffer.data();
    }

    void write_string(HKEY key, const wchar_t* name, std::wstring_view value) noexcept
    {
        RegSetValueExW(
            key,
            name,
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(value.data()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    }
}

namespace glance::app
{
    WindowPreferences load_window_preferences() noexcept
    {
        WindowPreferences preferences{
            .default_width = std::clamp<DWORD>(read_dword(L"DefaultWidth", 720), 480, 7680),
            .default_height = std::clamp<DWORD>(read_dword(L"DefaultHeight", 520), 320, 4320),
            .remember_size = read_dword(L"RememberSize", 1) != 0,
            .auto_fit_media = read_dword(L"AutoFitMedia", read_legacy_auto_fit() ? 1U : 0U) != 0,
            .show_after_auto_fit = read_dword(L"ShowAfterAutoFit", 0) != 0,
            .dynamic_auto_fit = read_dword(L"DynamicAutoFit", 0) != 0,
            .adaptive_minimum_percent = std::clamp<DWORD>(
                read_dword(L"AdaptiveMinimumPercent", 40), 10, 100),
            .adaptive_maximum_percent = std::clamp<DWORD>(
                read_dword(L"AdaptiveMaximumPercent", 75), 10, 100),
            .auto_fit_ignored_extensions = read_string(L"AutoFitIgnoredExtensions"),
            .remember_position = read_dword(L"RememberPosition", 0) != 0,
            .double_click_fullscreen = read_dword(L"DoubleClickFullscreen", 0) != 0,
        };
        preferences.adaptive_minimum_percent = std::min(
            preferences.adaptive_minimum_percent,
            preferences.adaptive_maximum_percent);
        return preferences;
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
        write_dword(key, L"ShowAfterAutoFit", preferences.show_after_auto_fit ? 1U : 0U);
        write_dword(key, L"DynamicAutoFit", preferences.dynamic_auto_fit ? 1U : 0U);
        write_dword(
            key,
            L"AdaptiveMinimumPercent",
            std::clamp<std::uint32_t>(preferences.adaptive_minimum_percent, 10, 100));
        write_dword(
            key,
            L"AdaptiveMaximumPercent",
            std::clamp<std::uint32_t>(preferences.adaptive_maximum_percent, 10, 100));
        write_string(key, L"AutoFitIgnoredExtensions", preferences.auto_fit_ignored_extensions);
        write_dword(key, L"RememberPosition", preferences.remember_position ? 1U : 0U);
        write_dword(
            key,
            L"DoubleClickFullscreen",
            preferences.double_click_fullscreen ? 1U : 0U);
        RegCloseKey(key);
    }

    bool auto_fit_ignores_path(
        const WindowPreferences& preferences,
        std::wstring_view path) noexcept
    {
        const auto separator = path.find_last_of(L"\\/");
        const auto dot = path.find_last_of(L'.');
        if (dot == std::wstring_view::npos ||
            (separator != std::wstring_view::npos && dot < separator) ||
            dot + 1 >= path.size())
        {
            return false;
        }

        const auto extension = path.substr(dot + 1);
        std::wstring_view remaining = preferences.auto_fit_ignored_extensions;
        while (!remaining.empty())
        {
            const auto delimiter = remaining.find(L';');
            auto token = remaining.substr(0, delimiter);
            while (!token.empty() && std::iswspace(token.front()))
            {
                token.remove_prefix(1);
            }
            while (!token.empty() && std::iswspace(token.back()))
            {
                token.remove_suffix(1);
            }
            if (!token.empty() && token.front() == L'.')
            {
                token.remove_prefix(1);
            }
            if (!token.empty() && token.size() == extension.size() &&
                _wcsnicmp(token.data(), extension.data(), extension.size()) == 0)
            {
                return true;
            }
            if (delimiter == std::wstring_view::npos)
            {
                break;
            }
            remaining.remove_prefix(delimiter + 1);
        }
        return false;
    }
}

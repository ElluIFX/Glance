#include "pch.h"
#include "appearance_preferences.h"

#include <winrt/Windows.UI.ViewManagement.h>

#include <algorithm>
#include <array>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
namespace Media = Microsoft::UI::Xaml::Media;

namespace
{
    constexpr wchar_t registry_path[] = L"Software\\Glance\\Appearance";

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

    Windows::UI::Color accent_color(glance::app::AccentPreference preference)
    {
        using glance::app::AccentPreference;
        switch (preference)
        {
        case AccentPreference::blue:
            return { 255, 0, 120, 212 };
        case AccentPreference::teal:
            return { 255, 0, 130, 114 };
        case AccentPreference::green:
            return { 255, 16, 124, 16 };
        case AccentPreference::orange:
            return { 255, 202, 80, 16 };
        case AccentPreference::red:
            return { 255, 209, 52, 56 };
        case AccentPreference::pink:
            return { 255, 227, 0, 140 };
        case AccentPreference::purple:
            return { 255, 92, 45, 145 };
        default:
            return Windows::UI::ViewManagement::UISettings().GetColorValue(
                Windows::UI::ViewManagement::UIColorType::Accent);
        }
    }

    Media::SolidColorBrush brush(Windows::UI::Color color, double opacity = 1.0)
    {
        Media::SolidColorBrush result(color);
        result.Opacity(opacity);
        return result;
    }

    template <typename T>
    void set_resource(ResourceDictionary const& resources, wchar_t const* key, T const& value)
    {
        resources.Insert(box_value(key), value);
    }
}

namespace glance::app
{
    AppearancePreferences load_appearance_preferences() noexcept
    {
        AppearancePreferences result;
        result.theme = static_cast<ThemePreference>(std::min<DWORD>(
            read_dword(L"Theme", 0),
            static_cast<DWORD>(ThemePreference::dark)));
        result.accent = static_cast<AccentPreference>(std::min<DWORD>(
            read_dword(L"Accent", 0),
            static_cast<DWORD>(AccentPreference::purple)));

        wchar_t language[32]{};
        DWORD size = sizeof(language);
        if (RegGetValueW(
                HKEY_CURRENT_USER,
                registry_path,
                L"Language",
                RRF_RT_REG_SZ,
                nullptr,
                language,
                &size) == ERROR_SUCCESS && language[0] != L'\0')
        {
            result.language = language;
        }
        return result;
    }

    void save_appearance_preferences(const AppearancePreferences& preferences) noexcept
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
        const DWORD theme = static_cast<DWORD>(preferences.theme);
        const DWORD accent = static_cast<DWORD>(preferences.accent);
        RegSetValueExW(key, L"Theme", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&theme), sizeof(theme));
        RegSetValueExW(key, L"Accent", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&accent), sizeof(accent));
        RegSetValueExW(
            key,
            L"Language",
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(preferences.language.c_str()),
            static_cast<DWORD>((preferences.language.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }

    ElementTheme element_theme(ThemePreference preference) noexcept
    {
        switch (preference)
        {
        case ThemePreference::light:
            return ElementTheme::Light;
        case ThemePreference::dark:
            return ElementTheme::Dark;
        default:
            return ElementTheme::Default;
        }
    }

    void apply_accent_resources(const AppearancePreferences& preferences)
    {
        const auto resources = Application::Current().Resources();
        const auto color = accent_color(preferences.accent);
        set_resource(resources, L"AccentFillColorDefaultBrush", brush(color));
        set_resource(resources, L"AccentFillColorSecondaryBrush", brush(color, 0.90));
        set_resource(resources, L"AccentFillColorTertiaryBrush", brush(color, 0.80));
        set_resource(resources, L"AccentTextFillColorPrimaryBrush", brush(color));
        set_resource(resources, L"AccentTextFillColorSecondaryBrush", brush(color, 0.90));
        set_resource(resources, L"AccentTextFillColorTertiaryBrush", brush(color, 0.80));
        set_resource(resources, L"ToggleButtonForegroundChecked", brush(color));
        set_resource(resources, L"ToggleButtonForegroundCheckedPointerOver", brush(color));
        set_resource(resources, L"ToggleButtonForegroundCheckedPressed", brush(color));
    }
}

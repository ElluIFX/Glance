#include "pch.h"
#include "text_preferences.h"

#include <dwrite.h>

#include <algorithm>
#include <ranges>

namespace
{
    constexpr wchar_t registry_path[] = L"Software\\Glance\\TextPreview";

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
}

namespace glance::app
{
    std::vector<std::wstring> system_font_families()
    {
        std::vector<std::wstring> result;
        winrt::com_ptr<IDWriteFactory> factory;
        if (FAILED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(factory.put_void()))))
        {
            return result;
        }
        winrt::com_ptr<IDWriteFontCollection> collection;
        if (FAILED(factory->GetSystemFontCollection(collection.put(), FALSE)))
        {
            return result;
        }
        wchar_t locale_name[LOCALE_NAME_MAX_LENGTH]{};
        GetUserDefaultLocaleName(locale_name, LOCALE_NAME_MAX_LENGTH);
        for (UINT32 index = 0; index < collection->GetFontFamilyCount(); ++index)
        {
            winrt::com_ptr<IDWriteFontFamily> family;
            winrt::com_ptr<IDWriteLocalizedStrings> names;
            if (FAILED(collection->GetFontFamily(index, family.put())) ||
                FAILED(family->GetFamilyNames(names.put())))
            {
                continue;
            }
            UINT32 name_index{};
            BOOL exists{};
            names->FindLocaleName(locale_name, &name_index, &exists);
            if (!exists)
            {
                names->FindLocaleName(L"en-us", &name_index, &exists);
            }
            if (!exists)
            {
                name_index = 0;
            }
            UINT32 length{};
            if (FAILED(names->GetStringLength(name_index, &length)))
            {
                continue;
            }
            std::wstring name(length + 1, L'\0');
            if (SUCCEEDED(names->GetString(name_index, name.data(), length + 1)))
            {
                name.resize(length);
                result.push_back(std::move(name));
            }
        }
        std::ranges::sort(result, [](const std::wstring& left, const std::wstring& right) {
            return _wcsicmp(left.c_str(), right.c_str()) < 0;
        });
        result.erase(std::unique(result.begin(), result.end(), [](const auto& left, const auto& right) {
            return _wcsicmp(left.c_str(), right.c_str()) == 0;
        }), result.end());
        return result;
    }

    TextPreferences load_text_preferences()
    {
        TextPreferences result;
        wchar_t font_family[LF_FACESIZE]{};
        DWORD size = sizeof(font_family);
        if (RegGetValueW(
                HKEY_CURRENT_USER,
                registry_path,
                L"FontFamily",
                RRF_RT_REG_SZ,
                nullptr,
                font_family,
                &size) == ERROR_SUCCESS && font_family[0] != L'\0')
        {
            result.font_family = font_family;
        }
        result.font_size = std::clamp(static_cast<double>(read_dword(L"FontSize", 9)), 7.0, 32.0);
        result.syntax_theme = static_cast<SyntaxThemePreference>(std::min<DWORD>(
            read_dword(L"SyntaxTheme", 0),
            static_cast<DWORD>(SyntaxThemePreference::material)));
        result.word_wrap = read_dword(L"WordWrap", 1) != 0;
        result.syntax_highlighting = read_dword(L"SyntaxHighlighting", 1) != 0;
        result.line_numbers = read_dword(L"LineNumbers", 1) != 0;
        return result;
    }

    void save_text_preferences(const TextPreferences& preferences) noexcept
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
        const auto font_size = static_cast<DWORD>(std::clamp(preferences.font_size, 7.0, 32.0));
        const DWORD syntax_theme = static_cast<DWORD>(preferences.syntax_theme);
        const DWORD word_wrap = preferences.word_wrap;
        const DWORD syntax_highlighting = preferences.syntax_highlighting;
        const DWORD line_numbers = preferences.line_numbers;
        RegSetValueExW(
            key,
            L"FontFamily",
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(preferences.font_family.c_str()),
            static_cast<DWORD>((preferences.font_family.size() + 1) * sizeof(wchar_t)));
        RegSetValueExW(key, L"FontSize", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&font_size), sizeof(font_size));
        RegSetValueExW(key, L"SyntaxTheme", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&syntax_theme), sizeof(syntax_theme));
        RegSetValueExW(key, L"WordWrap", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&word_wrap), sizeof(word_wrap));
        RegSetValueExW(key, L"SyntaxHighlighting", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&syntax_highlighting), sizeof(syntax_highlighting));
        RegSetValueExW(key, L"LineNumbers", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&line_numbers), sizeof(line_numbers));
        RegCloseKey(key);
    }
}

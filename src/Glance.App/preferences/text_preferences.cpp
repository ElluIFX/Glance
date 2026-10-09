#include "pch.h"
#include "glance/contracts/storage.h"
#include "text_preferences.h"
#include "public_settings.h"
#include "text_font_fallback.h"

#include <dwrite.h>

#include <algorithm>
#include <ranges>

namespace
{
    const glance::app::RegisterPublicSettings public_settings{
        { L"TextPreview/FontFamily", L"string", L"", 0, 31, L"", L"immediate" },
        { L"TextPreview/FontSize", L"integer", L"9", 7, 32, L"", L"immediate" },
        { L"TextPreview/SyntaxTheme", L"integer", L"0", 0, 17, L"", L"immediate" },
        { L"TextPreview/WordWrap", L"boolean", L"1", 0, 1, L"", L"immediate" },
        { L"TextPreview/SyntaxHighlighting", L"boolean", L"1", 0, 1, L"", L"immediate" },
        { L"TextPreview/LineNumbers", L"boolean", L"1", 0, 1, L"", L"immediate" },
        { L"TextPreview/MarkdownDefaultPreview", L"boolean", L"1", 0, 1, L"", L"next_preview" },
        { L"TextPreview/MarkdownFontFamily", L"string", L"Segoe UI", 0, 31, L"", L"immediate" },
        { L"TextPreview/MarkdownFontSize", L"integer", L"16", 8, 48, L"", L"immediate" },
        { L"TextPreview/MarkdownStyle", L"integer", L"0", 0, 6, L"", L"immediate" },
        { L"TextPreview/JsonDefaultTree", L"boolean", L"1", 0, 1, L"", L"next_preview" },
    };
    constexpr wchar_t registry_path[] = L"Software\\Glance\\TextPreview";

    DWORD read_dword(const wchar_t* name, DWORD fallback) noexcept
    {
        return glance::app::read_public_dword(registry_path, name, fallback);
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
        if (glance::contracts::storage::read_value(registry_path, L"FontFamily", REG_SZ, font_family, &size) == ERROR_SUCCESS && font_family[0] != L'\0')
        {
            result.font_family = font_family;
        }
        else
        {
            const auto font_families = system_font_families();
            result.font_family = select_default_text_font_family(font_families);
        }
        result.font_size = std::clamp(static_cast<double>(read_dword(L"FontSize", 9)), 7.0, 32.0);
        result.syntax_theme = static_cast<SyntaxThemePreference>(std::min<DWORD>(
            read_dword(L"SyntaxTheme", 0),
            static_cast<DWORD>(SyntaxThemePreference::material)));
        result.word_wrap = read_dword(L"WordWrap", 1) != 0;
        result.syntax_highlighting = read_dword(L"SyntaxHighlighting", 1) != 0;
        result.line_numbers = read_dword(L"LineNumbers", 1) != 0;
        result.markdown_default_preview = read_dword(L"MarkdownDefaultPreview", 1) != 0;
        result.json_default_tree = read_dword(L"JsonDefaultTree", 1) != 0;
        result.markdown_font_size = std::clamp(static_cast<double>(read_dword(L"MarkdownFontSize", 16)), 8.0, 48.0);
        result.markdown_style = std::min<DWORD>(read_dword(L"MarkdownStyle", 0), 6);
        size = sizeof(font_family);
        if (glance::contracts::storage::read_value(registry_path, L"MarkdownFontFamily", REG_SZ, font_family, &size) == ERROR_SUCCESS && font_family[0])
            result.markdown_font_family = font_family;
        return result;
    }

    void save_text_preferences(const TextPreferences& preferences) noexcept
    {
        glance::contracts::storage::Batch key(registry_path);
        const auto font_size = static_cast<DWORD>(std::clamp(preferences.font_size, 7.0, 32.0));
        const DWORD syntax_theme = static_cast<DWORD>(preferences.syntax_theme);
        const DWORD word_wrap = preferences.word_wrap;
        const DWORD syntax_highlighting = preferences.syntax_highlighting;
        const DWORD line_numbers = preferences.line_numbers;
        key.set(L"FontFamily", REG_SZ, reinterpret_cast<const BYTE*>(preferences.font_family.c_str()), static_cast<DWORD>((preferences.font_family.size() + 1) * sizeof(wchar_t)));
        key.set(L"FontSize", REG_DWORD, reinterpret_cast<const BYTE*>(&font_size), sizeof(font_size));
        key.set(L"SyntaxTheme", REG_DWORD, reinterpret_cast<const BYTE*>(&syntax_theme), sizeof(syntax_theme));
        key.set(L"WordWrap", REG_DWORD, reinterpret_cast<const BYTE*>(&word_wrap), sizeof(word_wrap));
        key.set(L"SyntaxHighlighting", REG_DWORD, reinterpret_cast<const BYTE*>(&syntax_highlighting), sizeof(syntax_highlighting));
        key.set(L"LineNumbers", REG_DWORD, reinterpret_cast<const BYTE*>(&line_numbers), sizeof(line_numbers));
        const auto write = [&key](const wchar_t* name, DWORD value) {
            key.set(name, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        };
        write(L"MarkdownDefaultPreview", preferences.markdown_default_preview);
        write(L"JsonDefaultTree", preferences.json_default_tree);
        write(L"MarkdownFontSize", static_cast<DWORD>(std::clamp(preferences.markdown_font_size, 8.0, 48.0)));
        write(L"MarkdownStyle", std::min<std::uint32_t>(preferences.markdown_style, 6));
        key.set(L"MarkdownFontFamily", REG_SZ, reinterpret_cast<const BYTE*>(preferences.markdown_font_family.c_str()), static_cast<DWORD>((preferences.markdown_font_family.size() + 1) * sizeof(wchar_t)));
        static_cast<void>(key.commit());
    }
}

#include "pch.h"
#include "folder_preview_preferences.h"
#include "public_settings.h"

namespace
{
    const glance::app::RegisterPublicSettings public_settings{
        { L"FolderPreview/SortField", L"integer", L"0", 0, 3, L"", L"next_preview" },
        { L"FolderPreview/SortAscending", L"boolean", L"1", 0, 1, L"", L"next_preview" },
    };
    constexpr wchar_t registry_path[] = L"Software\\Glance\\FolderPreview";

    DWORD read_dword(const wchar_t* name, DWORD fallback) noexcept
    {
        return glance::app::read_public_dword(registry_path, name, fallback);
    }
}

namespace glance::app
{
    FolderPreviewPreferences load_folder_preview_preferences() noexcept
    {
        const auto raw_field = read_dword(
            L"SortField",
            static_cast<DWORD>(FolderSortField::name));
        const auto maximum_field = static_cast<DWORD>(FolderSortField::size);
        return {
            .sort_field = static_cast<FolderSortField>(
                raw_field <= maximum_field
                    ? raw_field
                    : static_cast<DWORD>(FolderSortField::name)),
            .ascending = read_dword(L"SortAscending", 1) != 0,
        };
    }

    void save_folder_preview_preferences(const FolderPreviewPreferences& preferences) noexcept
    {
        const auto raw_field = static_cast<DWORD>(preferences.sort_field);
        if (raw_field > static_cast<DWORD>(FolderSortField::size))
        {
            return;
        }

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

        const DWORD ascending = preferences.ascending;
        RegSetValueExW(
            key,
            L"SortField",
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&raw_field),
            sizeof(raw_field));
        RegSetValueExW(
            key,
            L"SortAscending",
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&ascending),
            sizeof(ascending));
        RegCloseKey(key);
    }
}

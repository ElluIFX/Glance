#include "pch.h"
#include "folder_preview_preferences.h"

namespace
{
    constexpr wchar_t registry_path[] = L"Software\\Glance\\FolderPreview";

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

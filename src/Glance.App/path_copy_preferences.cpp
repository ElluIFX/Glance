#include "pch.h"
#include "path_copy_preferences.h"

namespace
{
    constexpr wchar_t registry_path[] = L"Software\\Glance\\PathCopy";

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
    PathCopyPreferences load_path_copy_preferences() noexcept
    {
        return {
            .quote_path = read_dword(L"QuotePath", 0) != 0,
            .use_unix_separators = read_dword(L"UseUnixSeparators", 0) != 0,
        };
    }

    void save_path_copy_preferences(const PathCopyPreferences& preferences) noexcept
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

        const DWORD quote_path = preferences.quote_path;
        const DWORD use_unix_separators = preferences.use_unix_separators;
        RegSetValueExW(
            key,
            L"QuotePath",
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&quote_path),
            sizeof(quote_path));
        RegSetValueExW(
            key,
            L"UseUnixSeparators",
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&use_unix_separators),
            sizeof(use_unix_separators));
        RegCloseKey(key);
    }
}

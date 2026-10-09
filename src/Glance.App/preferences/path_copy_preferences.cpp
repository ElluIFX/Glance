#include "pch.h"
#include "glance/contracts/storage.h"
#include "path_copy_preferences.h"
#include "public_settings.h"

namespace
{
    const glance::app::RegisterPublicSettings public_settings{
        { L"PathCopy/QuotePath", L"boolean", L"0", 0, 1, L"", L"immediate" },
        { L"PathCopy/UseUnixSeparators", L"boolean", L"0", 0, 1, L"", L"immediate" },
    };
    constexpr wchar_t registry_path[] = L"Software\\Glance\\PathCopy";

    DWORD read_dword(const wchar_t* name, DWORD fallback) noexcept
    {
        return glance::app::read_public_dword(registry_path, name, fallback);
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
        glance::contracts::storage::Batch key(registry_path);

        const DWORD quote_path = preferences.quote_path;
        const DWORD use_unix_separators = preferences.use_unix_separators;
        key.set(L"QuotePath", REG_DWORD, reinterpret_cast<const BYTE*>(&quote_path), sizeof(quote_path));
        key.set(L"UseUnixSeparators", REG_DWORD, reinterpret_cast<const BYTE*>(&use_unix_separators), sizeof(use_unix_separators));
        static_cast<void>(key.commit());
    }
}

#include "pch.h"
#include "glance/contracts/storage.h"
#include "update_preferences.h"
#include "public_settings.h"

#include <algorithm>

namespace
{
    const glance::app::RegisterPublicSettings public_settings{
        { L"Update/AutomaticCheckEnabled", L"boolean", L"1", 0, 1, L"", L"immediate" },
        { L"Update/CheckFrequency", L"integer", L"1", 0, 3, L"", L"immediate" },
    };
    constexpr wchar_t registry_path[] = L"Software\\Glance\\Update";

    DWORD read_dword(const wchar_t* name, DWORD fallback) noexcept
    {
        return glance::app::read_public_dword(registry_path, name, fallback);
    }

    std::uint64_t read_qword(const wchar_t* name) noexcept
    {
        std::uint64_t value{};
        DWORD size = sizeof(value);
        return glance::contracts::storage::read_value(registry_path, name, REG_QWORD, &value, &size) == ERROR_SUCCESS
            ? value
            : 0;
    }

    std::wstring read_string(const wchar_t* name) noexcept
    {
        wchar_t value[128]{};
        DWORD size = sizeof(value);
        if (glance::contracts::storage::read_value(registry_path, name, REG_SZ, value, &size) != ERROR_SUCCESS)
        {
            return {};
        }
        return value;
    }

    void write_dword(glance::contracts::storage::Batch& key, const wchar_t* name, DWORD value) noexcept
    {
        key.set(name, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    }

    void write_qword(glance::contracts::storage::Batch& key, const wchar_t* name, std::uint64_t value) noexcept
    {
        key.set(name, REG_QWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    }

    void write_string(glance::contracts::storage::Batch& key, const wchar_t* name, std::wstring_view value) noexcept
    {
        key.set(name, REG_SZ, reinterpret_cast<const BYTE*>(value.data()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    }
}

namespace glance::app
{
    UpdatePreferences load_update_preferences() noexcept
    {
        UpdatePreferences result;
        result.automatic_check_enabled = read_dword(L"AutomaticCheckEnabled", 1) != 0;
        result.frequency = static_cast<UpdateCheckFrequency>(std::min<DWORD>(
            read_dword(L"CheckFrequency", static_cast<DWORD>(UpdateCheckFrequency::daily)),
            static_cast<DWORD>(UpdateCheckFrequency::monthly)));
        result.last_successful_check = read_qword(L"LastSuccessfulCheck");
        result.retry_after = read_qword(L"RetryAfter");
        result.skipped_version = read_string(L"SkippedVersion");
        return result;
    }

    void save_update_preferences(const UpdatePreferences& preferences) noexcept
    {
        glance::contracts::storage::Batch key(registry_path);
        write_dword(key, L"AutomaticCheckEnabled", preferences.automatic_check_enabled ? 1U : 0U);
        write_dword(key, L"CheckFrequency", static_cast<DWORD>(preferences.frequency));
        write_qword(key, L"LastSuccessfulCheck", preferences.last_successful_check);
        write_qword(key, L"RetryAfter", preferences.retry_after);
        write_string(key, L"SkippedVersion", preferences.skipped_version);
        static_cast<void>(key.commit());
    }

}

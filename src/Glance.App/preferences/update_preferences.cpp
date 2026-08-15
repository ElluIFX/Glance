#include "pch.h"
#include "update_preferences.h"

#include <algorithm>

namespace
{
    constexpr wchar_t registry_path[] = L"Software\\Glance\\Update";

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

    std::uint64_t read_qword(const wchar_t* name) noexcept
    {
        std::uint64_t value{};
        DWORD size = sizeof(value);
        return RegGetValueW(
                   HKEY_CURRENT_USER,
                   registry_path,
                   name,
                   RRF_RT_REG_QWORD,
                   nullptr,
                   &value,
                   &size) == ERROR_SUCCESS
            ? value
            : 0;
    }

    std::wstring read_string(const wchar_t* name) noexcept
    {
        wchar_t value[128]{};
        DWORD size = sizeof(value);
        if (RegGetValueW(
                HKEY_CURRENT_USER,
                registry_path,
                name,
                RRF_RT_REG_SZ,
                nullptr,
                value,
                &size) != ERROR_SUCCESS)
        {
            return {};
        }
        return value;
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

    void write_qword(HKEY key, const wchar_t* name, std::uint64_t value) noexcept
    {
        RegSetValueExW(
            key,
            name,
            0,
            REG_QWORD,
            reinterpret_cast<const BYTE*>(&value),
            sizeof(value));
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
        write_dword(key, L"AutomaticCheckEnabled", preferences.automatic_check_enabled ? 1U : 0U);
        write_dword(key, L"CheckFrequency", static_cast<DWORD>(preferences.frequency));
        write_qword(key, L"LastSuccessfulCheck", preferences.last_successful_check);
        write_qword(key, L"RetryAfter", preferences.retry_after);
        write_string(key, L"SkippedVersion", preferences.skipped_version);
        RegCloseKey(key);
    }

}

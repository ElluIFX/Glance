#include "pch.h"
#include "startup_registration.h"

#include <shellapi.h>

#include <filesystem>
#include <string>
#include <vector>

namespace
{
    constexpr wchar_t run_key_path[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr wchar_t run_value_name[] = L"Glance";

    std::wstring executable_path()
    {
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size())
        {
            return {};
        }
        path.resize(length);
        return path;
    }

    std::wstring normalized_path(std::wstring_view path)
    {
        if (path.empty())
        {
            return {};
        }
        std::wstring input(path);
        const DWORD required = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
        if (required == 0)
        {
            return input;
        }
        std::wstring result(required, L'\0');
        const DWORD written = GetFullPathNameW(input.c_str(), required, result.data(), nullptr);
        if (written == 0 || written >= required)
        {
            return input;
        }
        result.resize(written);
        return result;
    }

    std::wstring read_run_command(HKEY root)
    {
        DWORD type{};
        DWORD size{};
        if (RegGetValueW(
                root,
                run_key_path,
                run_value_name,
                RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                &type,
                nullptr,
                &size) != ERROR_SUCCESS || size < sizeof(wchar_t))
        {
            return {};
        }

        std::wstring command(size / sizeof(wchar_t), L'\0');
        if (RegGetValueW(
                root,
                run_key_path,
                run_value_name,
                RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                &type,
                command.data(),
                &size) != ERROR_SUCCESS)
        {
            return {};
        }
        command.resize(size / sizeof(wchar_t));
        while (!command.empty() && command.back() == L'\0')
        {
            command.pop_back();
        }
        if (type != REG_EXPAND_SZ)
        {
            return command;
        }

        const DWORD expanded_size = ExpandEnvironmentStringsW(command.c_str(), nullptr, 0);
        if (expanded_size == 0)
        {
            return {};
        }
        std::wstring expanded(expanded_size, L'\0');
        if (ExpandEnvironmentStringsW(command.c_str(), expanded.data(), expanded_size) == 0)
        {
            return {};
        }
        while (!expanded.empty() && expanded.back() == L'\0')
        {
            expanded.pop_back();
        }
        return expanded;
    }

    bool command_targets(std::wstring_view command, std::wstring_view expected_path)
    {
        if (command.empty() || expected_path.empty())
        {
            return false;
        }
        int argument_count{};
        const std::wstring command_copy(command);
        LPWSTR* arguments = CommandLineToArgvW(command_copy.c_str(), &argument_count);
        if (arguments == nullptr || argument_count < 1)
        {
            LocalFree(arguments);
            return false;
        }
        const auto actual = normalized_path(arguments[0]);
        LocalFree(arguments);
        const auto expected = normalized_path(expected_path);
        return !actual.empty() && !expected.empty() &&
            CompareStringOrdinal(
                actual.c_str(),
                static_cast<int>(actual.size()),
                expected.c_str(),
                static_cast<int>(expected.size()),
                TRUE) == CSTR_EQUAL;
    }

    bool delete_matching_run_value(HKEY root, std::wstring_view expected_path)
    {
        const auto command = read_run_command(root);
        if (command.empty() || !command_targets(command, expected_path))
        {
            return true;
        }
        HKEY key{};
        if (RegOpenKeyExW(root, run_key_path, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        {
            return false;
        }
        const LSTATUS result = RegDeleteValueW(key, run_value_name);
        RegCloseKey(key);
        return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
    }
}

namespace glance::app
{
    bool launch_at_sign_in_enabled() noexcept
    {
        try
        {
            const auto path = executable_path();
            return !path.empty() && command_targets(read_run_command(HKEY_CURRENT_USER), path);
        }
        catch (...)
        {
            return false;
        }
    }

    bool set_launch_at_sign_in(bool enabled) noexcept
    {
        try
        {
            HKEY key{};
            if (RegCreateKeyExW(
                    HKEY_CURRENT_USER,
                    run_key_path,
                    0,
                    nullptr,
                    0,
                    KEY_SET_VALUE,
                    nullptr,
                    &key,
                    nullptr) != ERROR_SUCCESS)
            {
                return false;
            }

            LSTATUS result{};
            if (enabled)
            {
                const auto path = executable_path();
                if (path.empty())
                {
                    RegCloseKey(key);
                    return false;
                }
                const std::wstring command = L"\"" + path + L"\"";
                result = RegSetValueExW(
                    key,
                    run_value_name,
                    0,
                    REG_SZ,
                    reinterpret_cast<const BYTE*>(command.c_str()),
                    static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
            }
            else
            {
                result = RegDeleteValueW(key, run_value_name);
                if (result == ERROR_FILE_NOT_FOUND)
                {
                    result = ERROR_SUCCESS;
                }
            }
            RegCloseKey(key);
            return result == ERROR_SUCCESS;
        }
        catch (...)
        {
            return false;
        }
    }

    bool cleanup_launch_at_sign_in(std::wstring_view expected_path) noexcept
    {
        try
        {
            bool succeeded = delete_matching_run_value(HKEY_CURRENT_USER, expected_path);
            DWORD index{};
            std::vector<wchar_t> name(256);
            while (true)
            {
                DWORD length = static_cast<DWORD>(name.size());
                FILETIME written{};
                const LSTATUS enumeration = RegEnumKeyExW(
                    HKEY_USERS,
                    index,
                    name.data(),
                    &length,
                    nullptr,
                    nullptr,
                    nullptr,
                    &written);
                if (enumeration == ERROR_NO_MORE_ITEMS)
                {
                    break;
                }
                if (enumeration == ERROR_MORE_DATA)
                {
                    name.resize(name.size() * 2);
                    continue;
                }
                ++index;
                if (enumeration != ERROR_SUCCESS)
                {
                    succeeded = false;
                    continue;
                }

                HKEY user_root{};
                const std::wstring user_key(name.data(), length);
                if (RegOpenKeyExW(HKEY_USERS, user_key.c_str(), 0, KEY_READ, &user_root) == ERROR_SUCCESS)
                {
                    succeeded = delete_matching_run_value(user_root, expected_path) && succeeded;
                    RegCloseKey(user_root);
                }
            }
            return succeeded;
        }
        catch (...)
        {
            return false;
        }
    }
}

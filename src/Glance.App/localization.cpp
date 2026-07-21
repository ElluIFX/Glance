#include "pch.h"
#include "localization.h"

#include <filesystem>

namespace
{
    std::filesystem::path resource_file_path()
    {
        std::wstring executable_path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(
            nullptr,
            executable_path.data(),
            static_cast<DWORD>(executable_path.size()));
        executable_path.resize(length);
        return std::filesystem::path(executable_path).parent_path() / L"Glance.pri";
    }
}

namespace glance::app
{
    std::wstring localize(std::wstring_view key)
    {
        try
        {
            static const auto loader =
                winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader(
                    resource_file_path().wstring(),
                    L"Resources");
            const auto value = loader.GetString(key);
            return value.empty() ? std::wstring(key) : std::wstring(value);
        }
        catch (const winrt::hresult_error&)
        {
            return std::wstring(key);
        }
    }

    std::wstring localize_format(
        std::wstring_view key,
        std::initializer_list<std::wstring_view> arguments)
    {
        std::wstring result = localize(key);
        std::size_t index{};
        for (const auto argument : arguments)
        {
            const std::wstring token = L"{" + std::to_wstring(index++) + L"}";
            std::size_t position{};
            while ((position = result.find(token, position)) != std::wstring::npos)
            {
                result.replace(position, token.size(), argument);
                position += argument.size();
            }
        }
        return result;
    }
}

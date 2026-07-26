#include "pch.h"
#include "office_availability.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <string>

namespace
{
    enum OfficeComponent : unsigned int
    {
        word = 1U << 0U,
        excel = 1U << 1U,
        powerpoint = 1U << 2U,
    };

    std::once_flag initialization_flag;
    std::atomic_uint available_components{};

    bool read_default_value(std::wstring_view subkey, REGSAM view, std::wstring& value)
    {
        HKEY key{};
        if (RegOpenKeyExW(
                HKEY_CLASSES_ROOT,
                std::wstring(subkey).c_str(),
                0,
                KEY_QUERY_VALUE | view,
                &key) != ERROR_SUCCESS)
        {
            return false;
        }

        std::array<wchar_t, 1024> buffer{};
        DWORD type{};
        DWORD size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
        const LSTATUS status = RegQueryValueExW(
            key,
            nullptr,
            nullptr,
            &type,
            reinterpret_cast<BYTE*>(buffer.data()),
            &size);
        RegCloseKey(key);
        if (status != ERROR_SUCCESS ||
            (type != REG_SZ && type != REG_EXPAND_SZ) ||
            size <= sizeof(wchar_t))
        {
            return false;
        }

        buffer.back() = L'\0';
        value.assign(buffer.data());
        return !value.empty();
    }

    bool component_registered(std::wstring_view programmatic_id)
    {
        static constexpr std::array registry_views{ KEY_WOW64_64KEY, KEY_WOW64_32KEY };
        for (const REGSAM view : registry_views)
        {
            std::wstring class_id;
            if (!read_default_value(std::wstring(programmatic_id) + L"\\CLSID", view, class_id))
            {
                continue;
            }
            std::wstring local_server;
            if (read_default_value(L"CLSID\\" + class_id + L"\\LocalServer32", view, local_server))
            {
                return true;
            }
        }
        return false;
    }

    std::wstring lower_extension(std::wstring_view path)
    {
        auto extension = std::filesystem::path(path).extension().wstring();
        std::ranges::transform(extension, extension.begin(), [](wchar_t value) {
            return static_cast<wchar_t>(std::towlower(value));
        });
        return extension;
    }
}

namespace glance::app
{
    void initialize_office_availability() noexcept
    {
        try
        {
            std::call_once(initialization_flag, [] {
                unsigned int components{};
                try
                {
                    if (component_registered(L"Word.Application"))
                    {
                        components |= OfficeComponent::word;
                    }
                    if (component_registered(L"Excel.Application"))
                    {
                        components |= OfficeComponent::excel;
                    }
                    if (component_registered(L"PowerPoint.Application"))
                    {
                        components |= OfficeComponent::powerpoint;
                    }
                }
                catch (...)
                {
                    components = 0;
                }
                available_components.store(components, std::memory_order_release);
            });
        }
        catch (...)
        {
            available_components.store(0, std::memory_order_release);
        }
    }

    bool office_com_available() noexcept
    {
        initialize_office_availability();
        return available_components.load(std::memory_order_acquire) != 0;
    }

    unsigned int office_available_components() noexcept
    {
        initialize_office_availability();
        return available_components.load(std::memory_order_acquire);
    }

    bool office_preview_available(std::wstring_view path) noexcept
    {
        initialize_office_availability();
        const auto components = available_components.load(std::memory_order_acquire);
        std::wstring extension;
        try
        {
            extension = lower_extension(path);
        }
        catch (...)
        {
            return false;
        }
        if (extension == L".doc" || extension == L".docx")
        {
            return (components & OfficeComponent::word) != 0;
        }
        if (extension == L".xls" || extension == L".xlsx")
        {
            return (components & OfficeComponent::excel) != 0;
        }
        if (extension == L".ppt" || extension == L".pptx")
        {
            return (components & OfficeComponent::powerpoint) != 0;
        }
        return false;
    }
}

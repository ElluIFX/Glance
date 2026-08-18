#include "pch.h"
#include "office_availability.h"

#include <objbase.h>

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

    enum OfficeFormat : unsigned int
    {
        doc = 1U << 0U,
        docx = 1U << 1U,
        xls = 1U << 2U,
        xlsx = 1U << 3U,
        ppt = 1U << 4U,
        pptx = 1U << 5U,
    };

    std::once_flag initialization_flag;
    std::atomic_uint available_components{};
    std::atomic_uint available_formats{};

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

    bool preview_handler_registered(std::wstring_view extension)
    {
        std::wstring class_id;
        constexpr std::wstring_view preview_handler_key =
            L"\\shellex\\{8895b1c6-b41f-4c1c-a562-0d564250836f}";
        if (!read_default_value(
                std::wstring(extension) + std::wstring(preview_handler_key),
                KEY_WOW64_64KEY,
                class_id))
        {
            std::wstring programmatic_id;
            if (!read_default_value(extension, KEY_WOW64_64KEY, programmatic_id) ||
                !read_default_value(
                    programmatic_id + std::wstring(preview_handler_key),
                    KEY_WOW64_64KEY,
                    class_id))
            {
                return false;
            }
        }
        CLSID parsed{};
        return SUCCEEDED(CLSIDFromString(class_id.c_str(), &parsed));
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
                unsigned int formats{};
                try
                {
                    if (preview_handler_registered(L".doc")) formats |= OfficeFormat::doc;
                    if (preview_handler_registered(L".docx")) formats |= OfficeFormat::docx;
                    if (preview_handler_registered(L".xls")) formats |= OfficeFormat::xls;
                    if (preview_handler_registered(L".xlsx")) formats |= OfficeFormat::xlsx;
                    if (preview_handler_registered(L".ppt")) formats |= OfficeFormat::ppt;
                    if (preview_handler_registered(L".pptx")) formats |= OfficeFormat::pptx;
                    if ((formats & (OfficeFormat::doc | OfficeFormat::docx)) ==
                        (OfficeFormat::doc | OfficeFormat::docx))
                    {
                        components |= OfficeComponent::word;
                    }
                    if ((formats & (OfficeFormat::xls | OfficeFormat::xlsx)) ==
                        (OfficeFormat::xls | OfficeFormat::xlsx))
                    {
                        components |= OfficeComponent::excel;
                    }
                    if ((formats & (OfficeFormat::ppt | OfficeFormat::pptx)) ==
                        (OfficeFormat::ppt | OfficeFormat::pptx))
                    {
                        components |= OfficeComponent::powerpoint;
                    }
                }
                catch (...)
                {
                    components = 0;
                    formats = 0;
                }
                available_formats.store(formats, std::memory_order_release);
                available_components.store(components, std::memory_order_release);
            });
        }
        catch (...)
        {
            available_components.store(0, std::memory_order_release);
            available_formats.store(0, std::memory_order_release);
        }
    }

    unsigned int office_available_components() noexcept
    {
        initialize_office_availability();
        return available_components.load(std::memory_order_acquire);
    }

    bool office_preview_available(std::wstring_view path) noexcept
    {
        initialize_office_availability();
        const auto formats = available_formats.load(std::memory_order_acquire);
        std::wstring extension;
        try
        {
            extension = lower_extension(path);
        }
        catch (...)
        {
            return false;
        }
        if (extension == L".doc") return (formats & OfficeFormat::doc) != 0;
        if (extension == L".docx") return (formats & OfficeFormat::docx) != 0;
        if (extension == L".xls") return (formats & OfficeFormat::xls) != 0;
        if (extension == L".xlsx") return (formats & OfficeFormat::xlsx) != 0;
        if (extension == L".ppt") return (formats & OfficeFormat::ppt) != 0;
        if (extension == L".pptx") return (formats & OfficeFormat::pptx) != 0;
        return false;
    }
}

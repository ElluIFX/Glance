#include "pch.h"
#include "window_size_store.h"

#include <cstdint>

namespace
{
    constexpr wchar_t registry_path[] = L"Software\\Glance\\WindowSizes";

    const wchar_t* value_name(glance::app::PreviewKind kind) noexcept
    {
        using glance::app::PreviewKind;
        switch (kind)
        {
        case PreviewKind::text:
            return L"Text";
        case PreviewKind::markdown:
            return L"Markdown";
        case PreviewKind::image:
            return L"Image";
        case PreviewKind::media:
            return L"Media";
        case PreviewKind::pdf:
            return L"Pdf";
        case PreviewKind::archive:
            return L"Archive";
        case PreviewKind::office:
            return L"Office";
        default:
            return L"Generic";
        }
    }
}

namespace glance::app
{
    std::optional<SIZE> load_window_size(PreviewKind kind)
    {
        std::uint64_t packed{};
        DWORD size = sizeof(packed);
        if (RegGetValueW(
                HKEY_CURRENT_USER,
                registry_path,
                value_name(kind),
                RRF_RT_REG_QWORD,
                nullptr,
                &packed,
                &size) != ERROR_SUCCESS)
        {
            return std::nullopt;
        }

        SIZE result{
            static_cast<LONG>(packed >> 32U),
            static_cast<LONG>(packed & 0xFFFFFFFFU) };
        if (result.cx <= 0 || result.cy <= 0)
        {
            return std::nullopt;
        }
        return result;
    }

    void save_window_size(PreviewKind kind, SIZE size) noexcept
    {
        if (size.cx <= 0 || size.cy <= 0)
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

        const std::uint64_t packed =
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(size.cx)) << 32U) |
            static_cast<std::uint32_t>(size.cy);
        RegSetValueExW(
            key,
            value_name(kind),
            0,
            REG_QWORD,
            reinterpret_cast<const BYTE*>(&packed),
            sizeof(packed));
        RegCloseKey(key);
    }

    bool clear_window_sizes() noexcept
    {
        const LSTATUS result = RegDeleteTreeW(HKEY_CURRENT_USER, registry_path);
        return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND;
    }
}

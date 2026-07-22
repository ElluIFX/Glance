#include "pch.h"
#include "window_size_store.h"

#include <cstdint>

namespace
{
    constexpr wchar_t size_registry_path[] = L"Software\\Glance\\WindowSizes";
    constexpr wchar_t position_registry_path[] = L"Software\\Glance\\WindowPositions";

    const wchar_t* value_name(glance::app::PreviewKind kind, bool media_is_audio) noexcept
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
            return media_is_audio ? L"Audio" : L"Video";
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
    std::optional<SIZE> load_window_size(PreviewKind kind, bool media_is_audio)
    {
        std::uint64_t packed{};
        DWORD size = sizeof(packed);
        if (RegGetValueW(
                HKEY_CURRENT_USER,
                size_registry_path,
                value_name(kind, media_is_audio),
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

    void save_window_size(PreviewKind kind, SIZE size, bool media_is_audio) noexcept
    {
        if (size.cx <= 0 || size.cy <= 0)
        {
            return;
        }

        HKEY key{};
        if (RegCreateKeyExW(
                HKEY_CURRENT_USER,
                size_registry_path,
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
            value_name(kind, media_is_audio),
            0,
            REG_QWORD,
            reinterpret_cast<const BYTE*>(&packed),
            sizeof(packed));
        RegCloseKey(key);
    }

    bool clear_window_sizes() noexcept
    {
        const LSTATUS result = RegDeleteTreeW(HKEY_CURRENT_USER, size_registry_path);
        return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND;
    }

    std::optional<POINT> load_window_position(PreviewKind kind, bool media_is_audio)
    {
        std::uint64_t packed{};
        DWORD size = sizeof(packed);
        if (RegGetValueW(
            HKEY_CURRENT_USER,
            position_registry_path,
            value_name(kind, media_is_audio),
            RRF_RT_REG_QWORD,
            nullptr,
            &packed,
            &size) != ERROR_SUCCESS)
        {
            return std::nullopt;
        }
        return POINT{
            static_cast<LONG>(static_cast<std::int32_t>(packed >> 32U)),
            static_cast<LONG>(static_cast<std::int32_t>(packed & 0xFFFFFFFFU)) };
    }

    void save_window_position(PreviewKind kind, POINT position, bool media_is_audio) noexcept
    {
        HKEY key{};
        if (RegCreateKeyExW(
                HKEY_CURRENT_USER,
                position_registry_path,
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
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(position.x)) << 32U) |
            static_cast<std::uint32_t>(position.y);
        RegSetValueExW(
            key,
            value_name(kind, media_is_audio),
            0,
            REG_QWORD,
            reinterpret_cast<const BYTE*>(&packed),
            sizeof(packed));
        RegCloseKey(key);
    }

    bool clear_window_positions() noexcept
    {
        const LSTATUS result = RegDeleteTreeW(HKEY_CURRENT_USER, position_registry_path);
        return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND;
    }
}

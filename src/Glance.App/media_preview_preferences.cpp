#include "pch.h"
#include "media_preview_preferences.h"

#include <algorithm>

namespace
{
    constexpr wchar_t registry_path[] = L"Software\\Glance\\MediaPreview";

    DWORD read_volume(const wchar_t* name) noexcept
    {
        DWORD value{};
        DWORD size = sizeof(value);
        if (RegGetValueW(
                HKEY_CURRENT_USER,
                registry_path,
                name,
                RRF_RT_REG_DWORD,
                nullptr,
                &value,
                &size) != ERROR_SUCCESS)
        {
            return 100;
        }
        return std::min<DWORD>(value, 100);
    }

    void write_volume(HKEY key, const wchar_t* name, std::uint32_t volume) noexcept
    {
        const DWORD value = std::min<std::uint32_t>(volume, 100);
        RegSetValueExW(
            key,
            name,
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&value),
            sizeof(value));
    }
}

namespace glance::app
{
    MediaPreviewPreferences load_media_preview_preferences() noexcept
    {
        return {
            .audio_volume_percent = read_volume(L"AudioVolume"),
            .video_volume_percent = read_volume(L"VideoVolume"),
        };
    }

    void save_media_preview_preferences(const MediaPreviewPreferences& preferences) noexcept
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

        write_volume(key, L"AudioVolume", preferences.audio_volume_percent);
        write_volume(key, L"VideoVolume", preferences.video_volume_percent);
        RegCloseKey(key);
    }
}

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

    bool read_bool(const wchar_t* name, bool fallback) noexcept
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
            ? value != 0
            : fallback;
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

    void write_bool(HKEY key, const wchar_t* name, bool enabled) noexcept
    {
        const DWORD value = enabled ? 1U : 0U;
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
            .autoplay_audio = read_bool(L"AutoplayAudio", true),
            .autoplay_video = read_bool(L"AutoplayVideo", true),
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
        write_bool(key, L"AutoplayAudio", preferences.autoplay_audio);
        write_bool(key, L"AutoplayVideo", preferences.autoplay_video);
        RegCloseKey(key);
    }
}

#include "pch.h"
#include "media_preview_preferences.h"
#include "public_settings.h"

#include <algorithm>

namespace
{
    const glance::app::RegisterPublicSettings public_settings{
        { L"MediaPreview/AudioVolume", L"integer", L"100", 0, 100, L"", L"next_preview" },
        { L"MediaPreview/VideoVolume", L"integer", L"100", 0, 100, L"", L"next_preview" },
        { L"MediaPreview/AutoplayAudio", L"boolean", L"1", 0, 1, L"", L"next_preview" },
        { L"MediaPreview/AutoplayVideo", L"boolean", L"1", 0, 1, L"", L"next_preview" },
        { L"MediaPreview/ReverseSeekWheel", L"boolean", L"0", 0, 1, L"", L"next_preview" },
        { L"MediaPreview/MiddleClickGalleryMode", L"boolean", L"1", 0, 1, L"", L"next_preview" },
        { L"MediaPreview/LoopGalleryScrolling", L"boolean", L"1", 0, 1, L"", L"next_preview" },
        { L"MediaPreview/GallerySameExtensionOnly", L"boolean", L"0", 0, 1, L"", L"next_preview" },
        { L"MediaPreview/DisableAutoFitInGallery", L"boolean", L"0", 0, 1, L"", L"next_preview" },
        { L"MediaPreview/ShowImageZoomMap", L"boolean", L"1", 0, 1, L"", L"next_preview" },
    };
    constexpr wchar_t registry_path[] = L"Software\\Glance\\MediaPreview";

    DWORD read_volume(const wchar_t* name) noexcept
    {
        return glance::app::read_public_dword(registry_path, name, 100);
    }

    bool read_bool(const wchar_t* name, bool fallback) noexcept
    {
        return glance::app::read_public_dword(registry_path, name, fallback ? 1 : 0) != 0;
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
            .reverse_seek_wheel = read_bool(L"ReverseSeekWheel", false),
            .middle_click_gallery_mode = read_bool(L"MiddleClickGalleryMode", true),
            .loop_gallery_scrolling = read_bool(L"LoopGalleryScrolling", true),
            .gallery_same_extension_only = read_bool(L"GallerySameExtensionOnly", false),
            .disable_auto_fit_in_gallery = read_bool(L"DisableAutoFitInGallery", false),
            .show_image_zoom_map = read_bool(L"ShowImageZoomMap", true),
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
        write_bool(key, L"ReverseSeekWheel", preferences.reverse_seek_wheel);
        write_bool(key, L"MiddleClickGalleryMode", preferences.middle_click_gallery_mode);
        write_bool(key, L"LoopGalleryScrolling", preferences.loop_gallery_scrolling);
        write_bool(key, L"GallerySameExtensionOnly", preferences.gallery_same_extension_only);
        write_bool(key, L"DisableAutoFitInGallery", preferences.disable_auto_fit_in_gallery);
        write_bool(key, L"ShowImageZoomMap", preferences.show_image_zoom_map);
        RegCloseKey(key);
    }
}

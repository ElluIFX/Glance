#pragma once

#include <cstdint>

namespace glance::app
{
    inline constexpr std::uint32_t default_rich_document_render_dimension = 4096;

    [[nodiscard]] constexpr std::uint32_t normalize_rich_document_render_dimension(
        std::uint32_t value) noexcept
    {
        switch (value)
        {
        case 1024:
        case 2048:
        case 4096:
        case 8192:
            return value;
        default:
            return default_rich_document_render_dimension;
        }
    }

    struct MediaPreviewPreferences
    {
        std::uint32_t audio_volume_percent{ 100 };
        std::uint32_t video_volume_percent{ 100 };
        bool autoplay_audio{ true };
        bool autoplay_video{ true };
        bool reverse_seek_wheel{};
        bool show_image_zoom_map{ true };
    };

    [[nodiscard]] MediaPreviewPreferences load_media_preview_preferences() noexcept;
    void save_media_preview_preferences(const MediaPreviewPreferences& preferences) noexcept;
}

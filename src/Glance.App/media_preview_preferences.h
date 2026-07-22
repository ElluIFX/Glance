#pragma once

#include <cstdint>

namespace glance::app
{
    struct MediaPreviewPreferences
    {
        std::uint32_t audio_volume_percent{ 100 };
        std::uint32_t video_volume_percent{ 100 };
        bool autoplay_audio{ true };
        bool autoplay_video{ true };
    };

    [[nodiscard]] MediaPreviewPreferences load_media_preview_preferences() noexcept;
    void save_media_preview_preferences(const MediaPreviewPreferences& preferences) noexcept;
}

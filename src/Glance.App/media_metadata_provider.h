#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace glance::app
{
    struct MediaStreamMetadata
    {
        std::wstring type;
        std::wstring codec;
        std::wstring codec_description;
        std::wstring profile;
        std::wstring level;
        std::wstring width;
        std::wstring height;
        std::wstring frame_rate;
        std::wstring pixel_format;
        std::wstring bit_depth;
        std::wstring color;
        std::wstring aspect_ratio;
        std::wstring bitrate;
        std::wstring sample_rate;
        std::wstring sample_format;
        std::wstring channels;
        std::wstring channel_layout;
        std::wstring duration;
        std::wstring frame_count;
        std::wstring language;
        std::wstring title;
        std::wstring rotation;
        bool is_default{};
        bool is_forced{};
    };

    struct MediaTechnicalMetadata
    {
        std::wstring container;
        std::wstring duration;
        std::wstring overall_bitrate;
        std::uint32_t chapter_count{};
        std::vector<MediaStreamMetadata> streams;
        std::vector<std::pair<std::wstring, std::wstring>> tags;
    };

    [[nodiscard]] MediaTechnicalMetadata probe_media_metadata(std::wstring_view path) noexcept;
    [[nodiscard]] bool media_probe_available() noexcept;
    [[nodiscard]] std::wstring format_media_advanced_metadata(
        const MediaTechnicalMetadata& metadata);
}

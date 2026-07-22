#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace glance::app
{
    struct MediaTechnicalMetadata
    {
        std::wstring codec;
        double frame_rate{};
        std::uint64_t bitrate{};
        std::uint32_t sample_rate{};
        std::uint32_t bit_depth{};
    };

    [[nodiscard]] MediaTechnicalMetadata probe_media_metadata(
        std::wstring_view path,
        bool audio) noexcept;
    [[nodiscard]] bool media_probe_available() noexcept;
    [[nodiscard]] std::wstring format_media_metadata(
        const MediaTechnicalMetadata& metadata,
        bool audio);
}

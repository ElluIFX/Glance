#pragma once
#include <cstdint>

namespace glance::contracts::software_media
{
    inline constexpr std::uint32_t magic = 0x4D534C47;
    inline constexpr std::uint32_t version = 1;
    inline constexpr std::uint32_t maximum_sample_bytes = 128 * 1024 * 1024;
    enum class Command : std::uint32_t { open, sample_video, sample_audio, seek, close };
    struct Request
    {
        std::uint32_t signature{magic}, protocol{version};
        Command command{};
        std::uint32_t path_characters{};
        std::int64_t position{};
    };
    struct Response
    {
        std::uint32_t signature{magic}, protocol{version};
        std::int32_t error{};
        std::uint32_t bytes{};
        std::int64_t timestamp{}, duration{};
        std::uint32_t width{}, height{}, sample_rate{}, channels{};
        std::uint32_t frame_rate_numerator{}, frame_rate_denominator{1};
        std::uint32_t aspect_numerator{1}, aspect_denominator{1};
        std::uint32_t rotation{}, color_space{}, color_range{};
    };
}

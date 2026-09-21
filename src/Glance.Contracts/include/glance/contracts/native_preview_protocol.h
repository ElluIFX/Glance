#pragma once

#include <cstddef>
#include <cstdint>

namespace glance::contracts::native_preview
{
    inline constexpr std::uint32_t protocol_magic = 0x56504E47U;
    inline constexpr std::uint32_t protocol_version = 1U;
    inline constexpr std::uint32_t maximum_payload_size = 64U * 1024U;
    inline constexpr std::size_t media_setting_id_capacity = 64;

    enum class Command : std::uint32_t
    {
        open_document = 1,
        resize = 2,
        set_visuals = 3,
        unload = 4,
        shutdown = 5,
        media_play = 6,
        media_pause = 7,
        media_seek = 8,
        media_set_volume = 9,
        media_set_muted = 10,
        media_query_state = 11,
        media_set_view_mode = 12,
        media_set_settings = 13,
        query_content_size = 14,
    };

    enum class Status : std::uint32_t
    {
        success = 0,
        invalid_request = 1,
        open_failed = 2,
        handler_missing = 3,
        cancelled = 4,
        handler_creation_failed = 5,
        initialization_failed = 6,
        window_binding_failed = 7,
        preview_failed = 8,
        decoder_unavailable = 9,
        media_failed = 10,
    };

#pragma pack(push, 1)
    struct RequestHeader
    {
        std::uint32_t magic{ protocol_magic };
        std::uint32_t version{ protocol_version };
        Command command{};
        std::uint32_t payload_size{};
    };

    struct ResponseHeader
    {
        std::uint32_t magic{ protocol_magic };
        std::uint32_t version{ protocol_version };
        Status status{ Status::success };
        std::uint32_t payload_size{};
    };

    struct PreviewBounds
    {
        std::int32_t left{};
        std::int32_t top{};
        std::int32_t right{};
        std::int32_t bottom{};
    };

    struct PreviewVisuals
    {
        std::uint32_t background_color{};
        std::uint32_t text_color{};
        std::uint32_t color_scheme{};
    };

    struct OpenRequest
    {
        std::uint64_t parent_window{};
        PreviewBounds bounds{};
        PreviewVisuals visuals{};
        std::uint32_t dpi{ 96 };
        std::uint32_t path_characters{};
    };

    struct ResizeRequest
    {
        PreviewBounds bounds{};
        std::uint32_t dpi{ 96 };
    };

    // Preferred first-view dimensions in device-independent pixels; zero means unknown.
    struct ContentSize
    {
        std::uint32_t width{};
        std::uint32_t height{};
    };

    struct MediaValueRequest
    {
        std::int64_t value{};
    };

    struct MediaSettingsRequest
    {
        std::uint64_t generation{};
        std::uint32_t count{};
    };

    struct MediaSettingValue
    {
        wchar_t setting_id[media_setting_id_capacity]{};
        std::int64_t value{};
    };

    enum MediaStateFlags : std::uint32_t
    {
        media_state_ready = 1U << 0U,
        media_state_playing = 1U << 1U,
        media_state_muted = 1U << 2U,
        media_state_failed = 1U << 3U,
        media_state_projected_view = 1U << 4U,
    };

    struct MediaState
    {
        std::int64_t duration_ticks{};
        std::int64_t position_ticks{};
        std::uint32_t video_width{};
        std::uint32_t video_height{};
        std::uint32_t volume_percent{ 100 };
        std::uint32_t flags{};
        std::uint64_t interaction_generation{};
        std::uint32_t failure_kind{};
        std::int32_t failure_hresult{};
    };
#pragma pack(pop)
}

#pragma once

#include <cstdint>

namespace glance::contracts::native_preview
{
    inline constexpr std::uint32_t protocol_magic = 0x56504E47U;
    inline constexpr std::uint32_t protocol_version = 1U;
    inline constexpr std::uint32_t maximum_payload_size = 64U * 1024U;

    enum class Command : std::uint32_t
    {
        open_document = 1,
        resize = 2,
        set_visuals = 3,
        unload = 4,
        shutdown = 5,
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
#pragma pack(pop)
}

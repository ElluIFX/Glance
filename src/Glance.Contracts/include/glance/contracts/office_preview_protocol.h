#pragma once

#include <cstdint>

namespace glance::contracts::office
{
    inline constexpr std::uint32_t protocol_magic = 0x46464F47U;
    inline constexpr std::uint32_t protocol_version = 1U;
    inline constexpr std::uint32_t maximum_payload_size = 64U * 1024U;

    enum class Command : std::uint32_t
    {
        open_session = 1,
        render_page = 2,
        get_page_count = 3,
        shutdown = 4,
    };

    enum class Status : std::uint32_t
    {
        success = 0,
        invalid_request = 1,
        open_failed = 2,
        invalid_page = 3,
        render_failed = 4,
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

    struct PageRequest
    {
        std::uint32_t page_index{};
    };

    struct PageResponse
    {
        std::uint32_t page_index{};
        std::uint32_t path_characters{};
        float page_width_points{};
        float page_height_points{};
    };

    struct PageCountResponse
    {
        std::uint32_t page_count{};
    };
#pragma pack(pop)
}

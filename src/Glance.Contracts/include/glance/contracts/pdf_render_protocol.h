#pragma once

#include <cstdint>

namespace glance::contracts::pdf
{
    inline constexpr std::uint32_t protocol_magic = 0x46504447U;
    inline constexpr std::uint32_t protocol_version = 4U;
    inline constexpr std::uint32_t maximum_payload_size = 8U * 1024U * 1024U;
    inline constexpr std::uint32_t shared_bitmap_size = 256U * 1024U * 1024U;
    inline constexpr std::uint32_t maximum_bitmap_dimension = 8192U;

    enum class Command : std::uint32_t
    {
        open_document = 1,
        render_page = 2,
        shutdown = 3,
    };

    enum class Status : std::uint32_t
    {
        success = 0,
        invalid_request = 1,
        open_failed = 2,
        password_required = 3,
        invalid_password = 4,
        invalid_page = 5,
        render_failed = 6,
        dependency_missing = 7,
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

    struct OpenRequest
    {
        std::uint32_t path_characters{};
        std::uint32_t password_characters{};
    };

    struct OpenResponse
    {
        std::uint32_t page_count{};
        std::uint32_t outline_count{};
    };

    struct OutlineEntry
    {
        std::uint32_t depth{};
        std::int32_t page_index{ -1 };
        std::uint32_t title_characters{};
    };

    struct RenderRequest
    {
        std::uint32_t page_index{};
        std::uint32_t maximum_width{};
        std::uint32_t maximum_height{};
    };

    struct RenderResponse
    {
        std::uint32_t page_index{};
        std::uint32_t pixel_width{};
        std::uint32_t pixel_height{};
        std::uint32_t stride{};
        float page_width_points{};
        float page_height_points{};
    };
#pragma pack(pop)
}

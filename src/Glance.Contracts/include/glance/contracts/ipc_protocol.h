#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace glance::contracts
{
    inline constexpr std::uint32_t frame_magic = 0x434E4C47;
    inline constexpr std::uint16_t protocol_version = 1;
    inline constexpr std::uint32_t maximum_payload_size = 1024U * 1024U;
    inline constexpr wchar_t pipe_name[] = LR"(\\.\pipe\Glance.Core.v1)";

    enum class MessageType : std::uint16_t
    {
        hello = 1,
        hello_ack = 2,
        heartbeat = 3,
        heartbeat_ack = 4,
        open_active_preview = 10,
        close_active_preview = 11,
        preview_state_changed = 12,
        detach_preview = 13,
        shutdown = 20,
        terminate_unresponsive = 21,
    };

    struct FrameHeader
    {
        std::uint32_t magic{ frame_magic };
        std::uint16_t version{ protocol_version };
        MessageType type{};
        std::uint32_t flags{};
        std::uint32_t payload_size{};
        std::uint64_t correlation_id{};
    };

    [[nodiscard]] constexpr bool valid_header(const FrameHeader& header) noexcept
    {
        return header.magic == frame_magic && header.version == protocol_version &&
               header.payload_size <= maximum_payload_size;
    }
}

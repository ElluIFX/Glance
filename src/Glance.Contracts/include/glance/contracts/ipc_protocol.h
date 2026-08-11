#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace glance::contracts
{
    inline constexpr std::uint32_t frame_magic = 0x434E4C47;
    inline constexpr std::uint16_t protocol_version = 1;
    inline constexpr std::uint32_t maximum_payload_size = 1024U * 1024U;
    inline constexpr std::uint32_t process_watchdog_interval_ms = 500;
    inline constexpr std::uint32_t process_watchdog_failure_limit = 4;
    inline constexpr std::uint32_t process_watchdog_connect_grace_ms = 15000;
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
        preview_input = 14,
        gallery_request = 15,
        gallery_response = 16,
        source_status_request = 17,
        source_status_response = 18,
        shutdown = 20,
        terminate_unresponsive = 21,
    };

    // Heartbeat acknowledgements arrive on the pipe thread while the watchdog
    // checks on its own tick. A delayed acknowledgement may lag one round
    // behind the pending sequence; only an older acknowledgement (lag >= 2
    // rounds) or a stalled peer must count as a failure.
    [[nodiscard]] constexpr bool heartbeat_acknowledged(
        std::uint32_t pending_sequence,
        std::uint32_t acknowledged_sequence) noexcept
    {
        return pending_sequence - acknowledged_sequence <= 1U;
    }

    enum class PreviewInputAction : std::uint32_t
    {
        activate_selection = 1,
        navigate_back = 2,
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

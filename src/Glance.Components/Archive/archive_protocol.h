#pragma once

#include <cstdint>

namespace glance::components::archive
{
    inline constexpr std::uint32_t request_magic = 0x51524147;
    inline constexpr std::uint32_t response_magic = 0x52524147;
    inline constexpr std::uint32_t protocol_version = 1;
    inline constexpr std::uint32_t maximum_entries = 4000;
    inline constexpr std::uint32_t maximum_depth = 6;
    inline constexpr std::uint32_t maximum_response_bytes = 32U * 1024U * 1024U;

    enum class HostStatus : std::uint32_t
    {
        ready,
        password_required,
        invalid_password,
        unavailable,
        failed,
    };

    enum ResponseFlags : std::uint32_t
    {
        response_truncated = 1U << 0U,
        response_depth_limited = 1U << 1U,
        response_has_modified_time = 1U << 2U,
        response_has_packed_size = 1U << 3U,
        response_has_original_size = 1U << 4U,
        response_has_encrypted_items = 1U << 5U,
    };

    enum EntryFlags : std::uint32_t
    {
        entry_is_folder = 1U << 0U,
        entry_has_children = 1U << 1U,
        entry_has_modified_time = 1U << 2U,
        entry_has_packed_size = 1U << 3U,
        entry_has_original_size = 1U << 4U,
        entry_is_encrypted = 1U << 5U,
    };

    struct RequestHeader
    {
        std::uint32_t magic{ request_magic };
        std::uint32_t version{ protocol_version };
        std::uint32_t path_characters{};
        std::uint32_t password_characters{};
    };

    struct ResponseHeader
    {
        std::uint32_t magic{ response_magic };
        std::uint32_t version{ protocol_version };
        HostStatus status{ HostStatus::failed };
        std::uint32_t flags{};
        std::uint32_t format_name_characters{};
        std::uint32_t entry_count{};
        std::uint64_t file_count{};
        std::uint64_t packed_size{};
        std::uint64_t original_size{};
    };

    struct EntryHeader
    {
        std::uint64_t node_id{};
        std::uint64_t parent_id{};
        std::uint32_t flags{};
        std::uint32_t name_characters{};
        std::uint32_t type_characters{};
        std::uint64_t modified_time{};
        std::uint64_t packed_size{};
        std::uint64_t original_size{};
    };
}

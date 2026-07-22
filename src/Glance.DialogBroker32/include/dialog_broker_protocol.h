#pragma once

#include <cstdint>

namespace glance::dialog_broker
{
    inline constexpr std::uint32_t protocol_magic = 0x474C4442;
    inline constexpr std::uint32_t protocol_version = 1;
    inline constexpr std::uint32_t maximum_path_length = 32767;

    enum class Command : std::uint32_t
    {
        query = 1,
        detach = 2,
        shutdown = 3,
    };

    enum class Status : std::uint32_t
    {
        success = 0,
        no_selection = 1,
        hook_failed = 2,
        invalid_request = 3,
    };

    struct Request
    {
        std::uint32_t magic{ protocol_magic };
        std::uint32_t version{ protocol_version };
        Command command{};
        std::uint32_t reserved{};
        std::uint64_t window{};
        std::uint32_t process_id{};
        std::uint32_t thread_id{};
    };

    struct Response
    {
        std::uint32_t magic{ protocol_magic };
        std::uint32_t version{ protocol_version };
        Status status{ Status::invalid_request };
        std::uint32_t path_length{};
    };

    static_assert(sizeof(Request) == 32);
    static_assert(sizeof(Response) == 16);
}

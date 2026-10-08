#pragma once

#include <windows.h>
#include <atomic>
#include <cstdint>

namespace glance::contracts::access
{
    inline constexpr wchar_t service_name[] = L"Glance.Access";
    inline constexpr wchar_t service_pipe[] = LR"(\\.\pipe\Glance.Access.v1)";
    inline constexpr std::uint32_t magic = 0x41434C47;
    inline constexpr std::uint32_t version = 1;
    inline constexpr std::uint32_t maximum_payload = 1024 * 1024;

    struct InputState
    {
        std::atomic_bool ui_connected{};
        std::atomic_bool eligible_selection{};
        std::atomic_bool preview_active{};
        std::atomic_bool text_input_active{};
        std::atomic_bool enabled{true};
        std::atomic_uint64_t valid_until{};
    };

    struct SharedState
    {
        InputState input;
        std::atomic_uint64_t host_tick{};
        std::atomic_uint64_t hook_events{};
    };

    struct BootstrapRequest
    {
        std::uint32_t signature{magic};
        std::uint32_t protocol{version};
        wchar_t app_version[32]{};
        std::uint64_t mapping{};
        std::uint64_t notification_window{};
        std::uint32_t notification_message{};
    };

    struct BootstrapReply
    {
        DWORD error{};
        DWORD host_pid{};
        wchar_t endpoint[128]{};
    };

    enum class Operation : std::uint32_t { selection = 1, gallery, refresh_hook };

    struct Header
    {
        std::uint32_t signature{magic};
        Operation operation{};
        std::uint32_t size{};
        std::uint32_t flags{};
    };

    static_assert(std::atomic_uint64_t::is_always_lock_free);
}

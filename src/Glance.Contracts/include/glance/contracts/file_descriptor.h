#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace glance::contracts
{
    enum class HostKind : std::uint8_t
    {
        unsupported,
        explorer,
        common_dialog,
        everything,
        listary,
    };

    struct FileDescriptor
    {
        std::wstring display_name;
        std::wstring filesystem_path;
        std::wstring shell_parsing_name;
        std::uint64_t size{};
        std::uint64_t creation_time{};
        std::uint64_t last_write_time{};
        std::uint32_t attributes{};
        bool is_filesystem{};
        bool is_cloud_placeholder{};
        bool is_hydrated{};
    };

    struct SelectionSnapshot
    {
        std::uint64_t generation{};
        std::uint64_t timestamp_ms{};
        std::uintptr_t source_window{};
        std::uint32_t source_process_id{};
        HostKind host_kind{ HostKind::unsupported };
        bool accepts_hotkey{};
        std::vector<FileDescriptor> items;
        std::uint32_t focused_index{};
    };
}

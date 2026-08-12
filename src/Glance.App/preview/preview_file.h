#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace glance::app
{
    struct PreviewFile
    {
        std::wstring display_name;
        std::wstring path;
        std::wstring parsing_name;
        std::vector<std::uint8_t> shell_id_list;
        std::uint64_t size{};
        std::uint64_t creation_time{};
        std::uint64_t last_write_time{};
        std::uint32_t attributes{};
        bool is_filesystem{};
        bool is_cloud_placeholder{};
        std::shared_ptr<void> materialized_lease;
    };
}

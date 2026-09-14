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

    [[nodiscard]] inline bool same_filesystem_preview(
        const PreviewFile& current, const PreviewFile& next) noexcept
    {
        return current.is_filesystem && next.is_filesystem &&
            !current.path.empty() && current.path == next.path &&
            current.size == next.size &&
            current.creation_time == next.creation_time &&
            current.last_write_time == next.last_write_time &&
            current.attributes == next.attributes &&
            current.is_cloud_placeholder == next.is_cloud_placeholder &&
            current.display_name == next.display_name;
    }
}

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace glance::app
{
    struct ArchiveEntry
    {
        std::wstring name;
        std::uint64_t size{};
        bool is_folder{};
    };

    struct ArchivePreview
    {
        std::vector<ArchiveEntry> entries;
        std::wstring error;
        bool truncated{};
    };

    [[nodiscard]] ArchivePreview load_shell_archive_preview(
        const std::wstring& path,
        std::size_t maximum_entries = 5000);
}

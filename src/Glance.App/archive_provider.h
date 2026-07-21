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
        std::wstring type_name;
        std::uint64_t size{};
        std::uint64_t modified_time{};
        bool is_folder{};
    };

    struct ArchivePreview
    {
        std::vector<ArchiveEntry> entries;
        std::wstring error;
        std::uint64_t total_size{};
        bool show_total_size{};
        bool truncated{};
    };

    [[nodiscard]] ArchivePreview load_archive_preview(
        const std::wstring& path,
        std::size_t maximum_entries = 5000);
    [[nodiscard]] ArchivePreview load_shell_archive_preview(
        const std::wstring& path,
        std::size_t maximum_entries = 5000);
    [[nodiscard]] ArchivePreview load_directory_preview(
        const std::wstring& path,
        std::size_t maximum_entries = 5000);
}

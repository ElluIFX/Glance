#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace glance::app
{
    inline constexpr std::size_t maximum_preview_entries = 4000;

    struct ArchiveEntryValue
    {
        std::uint32_t kind{};
        std::uint64_t unsigned_value{};
        double ratio_value{};
        std::wstring text;
    };

    struct ArchiveEntry
    {
        std::wstring name;
        std::wstring path;
        std::wstring type_name;
        std::uint64_t compressed_size{};
        std::uint64_t original_size{};
        std::uint64_t creation_time{};
        std::uint64_t modified_time{};
        std::uint32_t attributes{};
        bool compressed_size_known{};
        bool original_size_known{};
        bool is_folder{};
        std::vector<ArchiveEntryValue> values;
        std::vector<ArchiveEntry> children;
    };

    struct ArchivePreview
    {
        std::vector<ArchiveEntry> entries;
        std::wstring error;
        std::size_t entry_count{};
        std::size_t file_count{};
        std::uint64_t compressed_size{};
        std::uint64_t original_size{};
        bool compressed_size_known{};
        bool original_size_known{};
        bool entry_compressed_size_available{};
        bool encrypted{};
        bool truncated{};
        bool entry_limit_reached{};
        bool depth_limited{};
    };

    [[nodiscard]] ArchivePreview load_directory_preview(
        const std::wstring& path,
        std::size_t maximum_entries = maximum_preview_entries);
}

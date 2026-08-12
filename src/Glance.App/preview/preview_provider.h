#pragma once

#include "glance/contracts/component_api.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace glance::app
{
    class IncrementalTextReader;

    enum class PreviewKind
    {
        generic,
        text,
        markdown,
        web,
        image,
        media,
        document,
        archive,
        component,
    };

    struct TextPreview
    {
        std::wstring content;
        std::wstring encoding;
        std::wstring error;
        std::shared_ptr<IncrementalTextReader> reader;
        bool has_more{};
    };

    struct MaterializedShellFile
    {
        std::wstring path;
        std::shared_ptr<void> lease;
        std::uint64_t size{};
        std::uint64_t creation_time{};
        std::uint64_t last_write_time{};
        std::int32_t error{};
        std::wstring error_stage;
        bool cancelled{};
    };

    enum class TextEncoding
    {
        automatic,
        utf8,
        utf16_le,
        utf16_be,
        gb2312,
        gbk,
        gb18030,
        big5,
        system,
    };

    [[nodiscard]] PreviewKind resolve_preview_kind(const std::wstring& path);
    [[nodiscard]] glance::contracts::components::GalleryMediaKind gallery_media_kind(
        const std::wstring& path);
    [[nodiscard]] std::vector<std::wstring> gallery_extensions(
        glance::contracts::components::GalleryMediaKind kind);
    [[nodiscard]] bool can_try_preview_as_text(const std::wstring& path);
    [[nodiscard]] MaterializedShellFile materialize_shell_file(
        std::wstring_view parsing_name,
        std::wstring_view display_name,
        std::span<const std::uint8_t> shell_id_list,
        const std::shared_ptr<std::atomic_bool>& cancellation) noexcept;
    [[nodiscard]] TextPreview load_text_preview(
        const std::wstring& path,
        std::size_t chunk_bytes = 256U * 1024U,
        TextEncoding encoding = TextEncoding::automatic);
    [[nodiscard]] TextPreview load_next_text_preview_chunk(
        const std::shared_ptr<IncrementalTextReader>& reader,
        std::size_t chunk_bytes = 256U * 1024U);
    void cancel_text_preview_read(
        const std::shared_ptr<IncrementalTextReader>& reader) noexcept;
}

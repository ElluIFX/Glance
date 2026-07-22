#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace glance::app
{
    class IncrementalTextReader;

    enum class PreviewKind
    {
        generic,
        text,
        markdown,
        image,
        media,
        pdf,
        archive,
        office,
    };

    struct TextPreview
    {
        std::wstring content;
        std::wstring encoding;
        std::wstring error;
        std::shared_ptr<IncrementalTextReader> reader;
        bool has_more{};
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
    [[nodiscard]] bool can_try_preview_as_text(const std::wstring& path);
    [[nodiscard]] TextPreview load_text_preview(
        const std::wstring& path,
        std::size_t chunk_bytes = 256U * 1024U,
        TextEncoding encoding = TextEncoding::automatic);
    [[nodiscard]] TextPreview load_next_text_preview_chunk(
        const std::shared_ptr<IncrementalTextReader>& reader,
        std::size_t chunk_bytes = 256U * 1024U);
}

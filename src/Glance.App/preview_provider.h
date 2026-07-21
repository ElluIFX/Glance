#pragma once

#include <cstddef>
#include <string>

namespace glance::app
{
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
        bool truncated{};
    };

    [[nodiscard]] PreviewKind resolve_preview_kind(const std::wstring& path);
    [[nodiscard]] TextPreview load_text_preview(
        const std::wstring& path,
        std::size_t maximum_bytes = 8U * 1024U * 1024U);
}

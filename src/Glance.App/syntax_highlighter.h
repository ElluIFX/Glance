#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace glance::app
{
    enum class SyntaxStyle
    {
        plain,
        keyword,
        string,
        comment,
        number,
        directive,
    };

    struct SyntaxSpan
    {
        std::size_t start{};
        std::size_t length{};
        SyntaxStyle style{};
    };

    enum class SyntaxContinuation
    {
        none,
        block_comment,
        markup_comment,
        line_comment,
        directive,
        markup_directive,
        string,
    };

    struct SyntaxHighlightState
    {
        SyntaxContinuation continuation{};
        wchar_t quote{};
        std::uint8_t continuation_prefix{};
        bool line_start{ true };
        bool string_escape_pending{};
    };

    [[nodiscard]] std::vector<SyntaxSpan> highlight_source(
        std::wstring_view text,
        std::wstring_view extension);
    [[nodiscard]] std::vector<SyntaxSpan> highlight_source_chunk(
        std::wstring_view text,
        std::wstring_view extension,
        SyntaxHighlightState& state);
}

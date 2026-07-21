#pragma once

#include <string>
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
        std::wstring text;
        SyntaxStyle style{};
    };

    [[nodiscard]] std::vector<SyntaxSpan> highlight_source(
        std::wstring_view text,
        std::wstring_view extension);
}

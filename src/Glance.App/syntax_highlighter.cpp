#include "pch.h"
#include "syntax_highlighter.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <unordered_set>

namespace
{
    bool is_identifier_start(wchar_t value)
    {
        return std::iswalpha(value) != 0 || value == L'_';
    }

    bool is_identifier_continue(wchar_t value)
    {
        return std::iswalnum(value) != 0 || value == L'_';
    }

    template <std::size_t Size>
    bool has_extension(
        std::wstring_view extension,
        const std::array<std::wstring_view, Size>& extensions)
    {
        return std::ranges::find(extensions, extension) != extensions.end();
    }

    void append_span(
        std::vector<glance::app::SyntaxSpan>& spans,
        std::size_t start,
        std::size_t length,
        glance::app::SyntaxStyle style)
    {
        if (length == 0)
        {
            return;
        }
        if (!spans.empty() && spans.back().style == style &&
            spans.back().start + spans.back().length == start)
        {
            spans.back().length += length;
            return;
        }
        spans.push_back({ start, length, style });
    }

    std::uint8_t trailing_terminator_prefix(
        std::wstring_view text,
        std::wstring_view terminator)
    {
        const auto maximum = std::min(text.size(), terminator.size() - 1U);
        for (std::size_t length = maximum; length > 0; --length)
        {
            if (text.substr(text.size() - length) == terminator.substr(0, length))
            {
                return static_cast<std::uint8_t>(length);
            }
        }
        return 0;
    }

    const std::unordered_set<std::wstring>& keywords(std::wstring_view extension)
    {
        static const std::unordered_set<std::wstring> empty;
        static const std::unordered_set<std::wstring> cpp{
            L"alignas", L"alignof", L"auto", L"bool", L"break", L"case", L"catch", L"char",
            L"class", L"const", L"constexpr", L"continue", L"delete", L"do", L"double", L"else",
            L"enum", L"false", L"float", L"for", L"friend", L"if", L"inline", L"int", L"long",
            L"namespace", L"new", L"nullptr", L"operator", L"override", L"private", L"protected",
            L"public", L"return", L"short", L"signed", L"sizeof", L"static", L"struct", L"switch",
            L"template", L"this", L"throw", L"true", L"try", L"typedef", L"typename", L"union",
            L"unsigned", L"using", L"virtual", L"void", L"volatile", L"while" };
        static const std::unordered_set<std::wstring> python{
            L"and", L"as", L"assert", L"async", L"await", L"break", L"class", L"continue",
            L"def", L"del", L"elif", L"else", L"except", L"False", L"finally", L"for", L"from",
            L"global", L"if", L"import", L"in", L"is", L"lambda", L"None", L"nonlocal", L"not",
            L"or", L"pass", L"raise", L"return", L"True", L"try", L"while", L"with", L"yield" };
        static const std::unordered_set<std::wstring> rust{
            L"as", L"async", L"await", L"break", L"const", L"continue", L"crate", L"dyn", L"else",
            L"enum", L"extern", L"false", L"fn", L"for", L"if", L"impl", L"in", L"let", L"loop",
            L"match", L"mod", L"move", L"mut", L"pub", L"ref", L"return", L"self", L"Self",
            L"static", L"struct", L"super", L"trait", L"true", L"type", L"unsafe", L"use", L"where",
            L"while" };
        static const std::unordered_set<std::wstring> go{
            L"break", L"case", L"chan", L"const", L"continue", L"default", L"defer", L"else",
            L"fallthrough", L"for", L"func", L"go", L"goto", L"if", L"import", L"interface",
            L"map", L"package", L"range", L"return", L"select", L"struct", L"switch", L"type", L"var" };
        static const std::unordered_set<std::wstring> java{
            L"abstract", L"assert", L"boolean", L"break", L"byte", L"case", L"catch", L"char",
            L"class", L"const", L"continue", L"default", L"do", L"double", L"else", L"enum",
            L"extends", L"false", L"final", L"finally", L"float", L"for", L"if", L"implements",
            L"import", L"instanceof", L"int", L"interface", L"long", L"native", L"new", L"null",
            L"package", L"private", L"protected", L"public", L"return", L"short", L"static", L"strictfp",
            L"super", L"switch", L"synchronized", L"this", L"throw", L"throws", L"transient", L"true",
            L"try", L"void", L"volatile", L"while" };
        static const std::unordered_set<std::wstring> javascript{
            L"async", L"await", L"break", L"case", L"catch", L"class", L"const", L"continue",
            L"debugger", L"default", L"delete", L"do", L"else", L"export", L"extends", L"false",
            L"finally", L"for", L"function", L"if", L"import", L"in", L"instanceof", L"let", L"new",
            L"null", L"return", L"static", L"super", L"switch", L"this", L"throw", L"true", L"try",
            L"typeof", L"undefined", L"var", L"void", L"while", L"with", L"yield" };
        static const std::unordered_set<std::wstring> shell{
            L"case", L"do", L"done", L"elif", L"else", L"esac", L"fi", L"for", L"function",
            L"if", L"in", L"select", L"then", L"until", L"while" };
        static const std::unordered_set<std::wstring> literals{
            L"false", L"null", L"true" };

        if (extension == L".c" || extension == L".h" || extension == L".cpp" ||
            extension == L".hpp" || extension == L".cc")
        {
            return cpp;
        }
        if (extension == L".py")
        {
            return python;
        }
        if (extension == L".rs")
        {
            return rust;
        }
        if (extension == L".go")
        {
            return go;
        }
        if (extension == L".java")
        {
            return java;
        }
        if (extension == L".js" || extension == L".ts" || extension == L".tsx" || extension == L".jsx")
        {
            return javascript;
        }
        if (extension == L".sh")
        {
            return shell;
        }
        if (extension == L".json" || extension == L".yaml" || extension == L".yml" ||
            extension == L".toml")
        {
            return literals;
        }
        return empty;
    }
}

namespace glance::app
{
    std::vector<SyntaxSpan> highlight_source_chunk(
        std::wstring_view text,
        std::wstring_view extension,
        SyntaxHighlightState& state)
    {
        std::vector<SyntaxSpan> spans;
        spans.reserve(std::min<std::size_t>(text.size() / 24, 8192));
        static constexpr std::array slash_comment_extensions{
            std::wstring_view(L".c"), std::wstring_view(L".h"), std::wstring_view(L".cpp"),
            std::wstring_view(L".hpp"), std::wstring_view(L".cc"), std::wstring_view(L".rs"),
            std::wstring_view(L".go"), std::wstring_view(L".java"), std::wstring_view(L".js"),
            std::wstring_view(L".ts"), std::wstring_view(L".tsx"), std::wstring_view(L".jsx") };
        static constexpr std::array block_comment_extensions{
            std::wstring_view(L".c"), std::wstring_view(L".h"), std::wstring_view(L".cpp"),
            std::wstring_view(L".hpp"), std::wstring_view(L".cc"), std::wstring_view(L".rs"),
            std::wstring_view(L".go"), std::wstring_view(L".java"), std::wstring_view(L".js"),
            std::wstring_view(L".ts"), std::wstring_view(L".tsx"), std::wstring_view(L".jsx"),
            std::wstring_view(L".css"), std::wstring_view(L".scss"), std::wstring_view(L".sql") };
        static constexpr std::array hash_comment_extensions{
            std::wstring_view(L".py"), std::wstring_view(L".sh"), std::wstring_view(L".ps1"),
            std::wstring_view(L".yaml"), std::wstring_view(L".yml"), std::wstring_view(L".toml"),
            std::wstring_view(L".cmake") };
        static constexpr std::array semicolon_comment_extensions{
            std::wstring_view(L".ini"), std::wstring_view(L".cfg"), std::wstring_view(L".conf") };
        static constexpr std::array preprocessor_extensions{
            std::wstring_view(L".c"), std::wstring_view(L".h"), std::wstring_view(L".cpp"),
            std::wstring_view(L".hpp"), std::wstring_view(L".cc") };
        static constexpr std::array markup_extensions{
            std::wstring_view(L".xml"), std::wstring_view(L".html"), std::wstring_view(L".htm"),
            std::wstring_view(L".xaml"), std::wstring_view(L".vcxproj") };

        const bool slash_comments = has_extension(extension, slash_comment_extensions);
        const bool block_comments = has_extension(extension, block_comment_extensions);
        const bool hash_comments = has_extension(extension, hash_comment_extensions);
        const bool semicolon_comments = has_extension(extension, semicolon_comment_extensions);
        const bool preprocessor = has_extension(extension, preprocessor_extensions);
        const bool sql_comments = extension == L".sql";
        const bool markup = has_extension(extension, markup_extensions);
        const auto& language_keywords = keywords(extension);
        for (std::size_t index = 0; index < text.size();)
        {
            const std::size_t start = index;
            if (state.continuation == SyntaxContinuation::block_comment ||
                state.continuation == SyntaxContinuation::markup_comment)
            {
                const auto terminator = state.continuation == SyntaxContinuation::block_comment
                    ? std::wstring_view(L"*/")
                    : std::wstring_view(L"-->");
                if (state.continuation_prefix > 0)
                {
                    const auto remainder = terminator.substr(state.continuation_prefix);
                    if (text.substr(index).starts_with(remainder))
                    {
                        index += remainder.size();
                        append_span(spans, start, index - start, SyntaxStyle::comment);
                        state.continuation = SyntaxContinuation::none;
                        state.continuation_prefix = 0;
                        continue;
                    }
                    state.continuation_prefix = 0;
                }
                const auto end = text.find(terminator, index);
                index = end == std::wstring_view::npos
                    ? text.size()
                    : end + terminator.size();
                append_span(spans, start, index - start, SyntaxStyle::comment);
                state.line_start = index > start && text[index - 1] == L'\n';
                if (end != std::wstring_view::npos)
                {
                    state.continuation = SyntaxContinuation::none;
                    state.continuation_prefix = 0;
                }
                else
                {
                    state.continuation_prefix = trailing_terminator_prefix(
                        text.substr(start, index - start),
                        terminator);
                }
                continue;
            }
            if (state.continuation == SyntaxContinuation::line_comment ||
                state.continuation == SyntaxContinuation::directive)
            {
                const auto end = text.find(L'\n', index);
                index = end == std::wstring_view::npos ? text.size() : end + 1;
                append_span(
                    spans,
                    start,
                    index - start,
                    state.continuation == SyntaxContinuation::line_comment
                        ? SyntaxStyle::comment
                        : SyntaxStyle::directive);
                state.line_start = end != std::wstring_view::npos;
                if (end != std::wstring_view::npos)
                {
                    state.continuation = SyntaxContinuation::none;
                }
                continue;
            }
            if (state.continuation == SyntaxContinuation::markup_directive)
            {
                const auto end = text.find(L'>', index);
                index = end == std::wstring_view::npos ? text.size() : end + 1;
                append_span(spans, start, index - start, SyntaxStyle::directive);
                state.line_start = index > start && text[index - 1] == L'\n';
                if (end != std::wstring_view::npos)
                {
                    state.continuation = SyntaxContinuation::none;
                }
                continue;
            }
            if (state.continuation == SyntaxContinuation::string)
            {
                if (state.string_escape_pending && index < text.size())
                {
                    ++index;
                    state.string_escape_pending = false;
                }
                while (index < text.size())
                {
                    if (text[index] == L'\\')
                    {
                        if (index + 1 < text.size())
                        {
                            index += 2;
                        }
                        else
                        {
                            ++index;
                            state.string_escape_pending = true;
                        }
                        continue;
                    }
                    const wchar_t value = text[index++];
                    if (value == state.quote || value == L'\n')
                    {
                        state.continuation = SyntaxContinuation::none;
                        state.string_escape_pending = false;
                        break;
                    }
                }
                append_span(spans, start, index - start, SyntaxStyle::string);
                state.line_start = index > start && text[index - 1] == L'\n';
                continue;
            }
            if (markup && text.substr(index).starts_with(L"<!--"))
            {
                const auto end = text.find(L"-->", index + 4);
                index = end == std::wstring_view::npos ? text.size() : end + 3;
                append_span(spans, start, index - start, SyntaxStyle::comment);
                state.line_start = index > start && text[index - 1] == L'\n';
                state.continuation = end == std::wstring_view::npos
                    ? SyntaxContinuation::markup_comment
                    : SyntaxContinuation::none;
                state.continuation_prefix = end == std::wstring_view::npos
                    ? trailing_terminator_prefix(text.substr(start, index - start), L"-->")
                    : 0;
                continue;
            }
            if (block_comments && text.substr(index).starts_with(L"/*"))
            {
                const auto end = text.find(L"*/", index + 2);
                index = end == std::wstring_view::npos ? text.size() : end + 2;
                append_span(spans, start, index - start, SyntaxStyle::comment);
                state.line_start = index > start && text[index - 1] == L'\n';
                state.continuation = end == std::wstring_view::npos
                    ? SyntaxContinuation::block_comment
                    : SyntaxContinuation::none;
                state.continuation_prefix = end == std::wstring_view::npos
                    ? trailing_terminator_prefix(text.substr(start, index - start), L"*/")
                    : 0;
                continue;
            }
            const bool slash_comment = slash_comments && text.substr(index).starts_with(L"//");
            const bool special_comment = (hash_comments && text[index] == L'#') ||
                (semicolon_comments && text[index] == L';') ||
                (sql_comments && text.substr(index).starts_with(L"--"));
            if (slash_comment || special_comment)
            {
                const auto end = text.find(L'\n', index);
                index = end == std::wstring_view::npos ? text.size() : end + 1;
                append_span(spans, start, index - start, SyntaxStyle::comment);
                state.line_start = end != std::wstring_view::npos;
                state.continuation = end == std::wstring_view::npos
                    ? SyntaxContinuation::line_comment
                    : SyntaxContinuation::none;
                continue;
            }
            if (preprocessor && state.line_start && text[index] == L'#')
            {
                const auto end = text.find(L'\n', index);
                index = end == std::wstring_view::npos ? text.size() : end + 1;
                append_span(spans, start, index - start, SyntaxStyle::directive);
                state.line_start = end != std::wstring_view::npos;
                state.continuation = end == std::wstring_view::npos
                    ? SyntaxContinuation::directive
                    : SyntaxContinuation::none;
                continue;
            }
            if (markup && text[index] == L'<')
            {
                const auto end = text.find(L'>', index + 1);
                index = end == std::wstring_view::npos ? text.size() : end + 1;
                append_span(spans, start, index - start, SyntaxStyle::directive);
                state.line_start = false;
                state.continuation = end == std::wstring_view::npos
                    ? SyntaxContinuation::markup_directive
                    : SyntaxContinuation::none;
                continue;
            }
            if (text[index] == L'\'' || text[index] == L'"' || text[index] == L'`')
            {
                const wchar_t quote = text[index++];
                bool closed{};
                while (index < text.size())
                {
                    if (text[index] == L'\\')
                    {
                        if (index + 1 < text.size())
                        {
                            index += 2;
                        }
                        else
                        {
                            ++index;
                            state.string_escape_pending = true;
                        }
                        continue;
                    }
                    const wchar_t value = text[index++];
                    if (value == quote || value == L'\n')
                    {
                        closed = true;
                        break;
                    }
                }
                append_span(spans, start, index - start, SyntaxStyle::string);
                state.line_start = index > start && text[index - 1] == L'\n';
                if (!closed)
                {
                    state.continuation = SyntaxContinuation::string;
                    state.quote = quote;
                }
                continue;
            }
            if (std::iswdigit(text[index]) != 0)
            {
                ++index;
                while (index < text.size() &&
                       (std::iswalnum(text[index]) != 0 || text[index] == L'.' || text[index] == L'_'))
                {
                    ++index;
                }
                append_span(spans, start, index - start, SyntaxStyle::number);
                state.line_start = false;
                continue;
            }
            if (is_identifier_start(text[index]))
            {
                ++index;
                while (index < text.size() && is_identifier_continue(text[index]))
                {
                    ++index;
                }
                const std::wstring token(text.substr(start, index - start));
                append_span(
                    spans,
                    start,
                    index - start,
                    language_keywords.contains(token) ? SyntaxStyle::keyword : SyntaxStyle::plain);
                state.line_start = false;
                continue;
            }

            ++index;
            const wchar_t value = text[start];
            append_span(spans, start, 1, SyntaxStyle::plain);
            if (value == L'\n')
            {
                state.line_start = true;
            }
            else if (std::iswspace(value) == 0)
            {
                state.line_start = false;
            }
        }
        return spans;
    }

    std::vector<SyntaxSpan> highlight_source(
        std::wstring_view text,
        std::wstring_view extension)
    {
        SyntaxHighlightState state;
        return highlight_source_chunk(text, extension, state);
    }
}

#include "pch.h"
#include "syntax_highlighter.h"

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

    void append_span(
        std::vector<glance::app::SyntaxSpan>& spans,
        std::wstring_view text,
        glance::app::SyntaxStyle style)
    {
        if (text.empty())
        {
            return;
        }
        if (!spans.empty() && spans.back().style == style)
        {
            spans.back().text.append(text);
            return;
        }
        spans.push_back({ std::wstring(text), style });
    }

    const std::unordered_set<std::wstring>& keywords()
    {
        static const std::unordered_set<std::wstring> values{
            L"alignas", L"alignof", L"and", L"as", L"async", L"auto", L"await", L"bool",
            L"break", L"case", L"catch", L"char", L"class", L"const", L"constexpr", L"continue",
            L"def", L"delete", L"do", L"double", L"else", L"enum", L"except", L"export", L"extends",
            L"false", L"final", L"finally", L"float", L"for", L"from", L"func", L"function", L"if",
            L"implements", L"import", L"in", L"inline", L"int", L"interface", L"let", L"long",
            L"match", L"namespace", L"new", L"nullptr", L"null", L"operator", L"override", L"package",
            L"pass", L"private", L"protected", L"public", L"raise", L"return", L"self", L"short",
            L"signed", L"sizeof", L"static", L"struct", L"super", L"switch", L"template", L"this",
            L"throw", L"true", L"try", L"typedef", L"typename", L"union", L"unsigned", L"using",
            L"var", L"virtual", L"void", L"volatile", L"while", L"with", L"yield" };
        return values;
    }
}

namespace glance::app
{
    std::vector<SyntaxSpan> highlight_source(std::wstring_view text, std::wstring_view extension)
    {
        std::vector<SyntaxSpan> spans;
        spans.reserve(std::min<std::size_t>(text.size() / 24, 8192));
        const bool hash_comments = extension == L".py" || extension == L".sh" || extension == L".ps1" ||
            extension == L".yaml" || extension == L".yml" || extension == L".toml";
        const bool semicolon_comments = extension == L".ini" || extension == L".cfg" || extension == L".conf";
        const bool sql_comments = extension == L".sql";
        const bool markup = extension == L".xml" || extension == L".html" || extension == L".htm" ||
            extension == L".xaml";
        bool line_start{ true };

        for (std::size_t index = 0; index < text.size();)
        {
            const std::size_t start = index;
            if (text.substr(index).starts_with(L"/*"))
            {
                const auto end = text.find(L"*/", index + 2);
                index = end == std::wstring_view::npos ? text.size() : end + 2;
                append_span(spans, text.substr(start, index - start), SyntaxStyle::comment);
                continue;
            }
            const bool slash_comment = text.substr(index).starts_with(L"//");
            const bool special_comment = (hash_comments && text[index] == L'#') ||
                (semicolon_comments && text[index] == L';') ||
                (sql_comments && text.substr(index).starts_with(L"--"));
            if (slash_comment || special_comment)
            {
                const auto end = text.find(L'\n', index);
                index = end == std::wstring_view::npos ? text.size() : end;
                append_span(spans, text.substr(start, index - start), SyntaxStyle::comment);
                continue;
            }
            if (!hash_comments && line_start && text[index] == L'#')
            {
                const auto end = text.find(L'\n', index);
                index = end == std::wstring_view::npos ? text.size() : end;
                append_span(spans, text.substr(start, index - start), SyntaxStyle::directive);
                continue;
            }
            if (markup && text[index] == L'<')
            {
                const auto end = text.find(L'>', index + 1);
                index = end == std::wstring_view::npos ? text.size() : end + 1;
                append_span(spans, text.substr(start, index - start), SyntaxStyle::directive);
                line_start = false;
                continue;
            }
            if (text[index] == L'\'' || text[index] == L'"' || text[index] == L'`')
            {
                const wchar_t quote = text[index++];
                while (index < text.size())
                {
                    if (text[index] == L'\\' && index + 1 < text.size())
                    {
                        index += 2;
                        continue;
                    }
                    const wchar_t value = text[index++];
                    if (value == quote || value == L'\n')
                    {
                        break;
                    }
                }
                append_span(spans, text.substr(start, index - start), SyntaxStyle::string);
                line_start = false;
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
                append_span(spans, text.substr(start, index - start), SyntaxStyle::number);
                line_start = false;
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
                    token,
                    keywords().contains(token) ? SyntaxStyle::keyword : SyntaxStyle::plain);
                line_start = false;
                continue;
            }

            ++index;
            const wchar_t value = text[start];
            append_span(spans, text.substr(start, 1), SyntaxStyle::plain);
            if (value == L'\n')
            {
                line_start = true;
            }
            else if (std::iswspace(value) == 0)
            {
                line_start = false;
            }
        }
        return spans;
    }
}

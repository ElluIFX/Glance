#include "pch.h"
#include "markdown_renderer.h"
#include "third_party/md4c/md4c-html.h"

#include <limits>

namespace
{
    std::string to_utf8(std::wstring_view value)
    {
        if (value.empty())
        {
            return {};
        }
        const int size = WideCharToMultiByte(
            CP_UTF8,
            0,
            value.data(),
            static_cast<int>(value.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        std::string result(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(
            CP_UTF8,
            0,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            size,
            nullptr,
            nullptr);
        return result;
    }

    void append_html(const MD_CHAR* text, MD_SIZE size, void* context)
    {
        static_cast<std::string*>(context)->append(text, size);
    }

    std::string document_prefix(bool dark_theme)
    {
        const char* colors = dark_theme
            ? "color-scheme:dark;--fg:#f2f2f2;--muted:#a8a8a8;--border:#4a4a4a;--surface:#202020;--quote:#a8a8a8;"
            : "color-scheme:light;--fg:#1b1b1b;--muted:#606060;--border:#d0d0d0;--surface:#f3f3f3;--quote:#606060;";
        std::string result =
            "<!doctype html><html><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<style>:root{";
        result += colors;
        result +=
            "font-family:'Segoe UI',sans-serif;font-size:14px;}"
            "*{box-sizing:border-box;}"
            "html{background:transparent;color:var(--fg);}"
            "body{margin:0;padding:16px 28px 32px;max-width:956px;line-height:1.55;overflow-wrap:anywhere;}"
            "h1,h2,h3,h4,h5,h6{line-height:1.25;margin:1.35em 0 .55em;font-weight:600;}"
            "h1{font-size:2em;border-bottom:1px solid var(--border);padding-bottom:.25em;}"
            "h2{font-size:1.5em;border-bottom:1px solid var(--border);padding-bottom:.2em;}"
            "h3{font-size:1.25em;}p,ul,ol,blockquote,pre,table{margin:.75em 0;}"
            "a{color:#479ef5;text-decoration:none;}a:hover{text-decoration:underline;}"
            "code,pre{font-family:'Cascadia Mono','Consolas',monospace;}"
            "code{background:var(--surface);border-radius:3px;padding:.15em .32em;}"
            "pre{background:var(--surface);border:1px solid var(--border);border-radius:5px;padding:12px 14px;overflow:auto;}"
            "pre code{background:transparent;padding:0;}"
            "blockquote{border-left:3px solid var(--border);color:var(--quote);margin-left:0;padding-left:14px;}"
            "table{border-collapse:collapse;display:block;max-width:100%;overflow:auto;}"
            "th,td{border:1px solid var(--border);padding:6px 12px;text-align:left;}"
            "th{background:var(--surface);font-weight:600;}"
            "tr:nth-child(even) td{background:var(--surface);}"
            "img{max-width:100%;height:auto;}hr{border:0;border-top:1px solid var(--border);}"
            "input[type=checkbox]{accent-color:#479ef5;margin-right:.45em;}"
            ".admonition{border-left:3px solid #479ef5;padding-left:14px;}"
            "</style></head><body>";
        return result;
    }
}

namespace glance::app
{
    std::wstring render_markdown_html(std::wstring_view markdown, bool dark_theme)
    {
        const std::string input = to_utf8(markdown);
        std::string html = document_prefix(dark_theme);
        const int result = md_html(
            input.data(),
            static_cast<MD_SIZE>(std::min<std::size_t>(input.size(), std::numeric_limits<MD_SIZE>::max())),
            append_html,
            &html,
            MD_DIALECT_GITHUB | MD_FLAG_NOHTML,
            MD_HTML_FLAG_SKIP_UTF8_BOM);
        if (result != 0)
        {
            html += "<p>Markdown rendering failed.</p>";
        }
        html += "</body></html>";
        return winrt::to_hstring(html).c_str();
    }
}

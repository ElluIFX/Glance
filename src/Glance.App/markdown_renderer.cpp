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
            ? "color-scheme:dark;--fg:#f0f6fc;--muted:#9198a1;--border:#3d444d;--surface:#151b23;--accent:#4493f8;"
            : "color-scheme:light;--fg:#1f2328;--muted:#59636e;--border:#d1d9e0;--surface:#f6f8fa;--accent:#0969da;";
        std::string result =
            "<!doctype html><html><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<base href=\"https://glance-markdown-assets.invalid/\">"
            "<style>:root{";
        result += colors;
        result +=
            "font-family:'Segoe UI','Arial',sans-serif;font-size:16px;}"
            "*{box-sizing:border-box;}"
            "html{background:transparent;color:var(--fg);}"
            "body{margin:0 auto;padding:32px;max-width:1012px;line-height:1.5;overflow-wrap:break-word;}"
            "body>*:first-child{margin-top:0!important;}body>*:last-child{margin-bottom:0!important;}"
            "h1,h2,h3,h4,h5,h6{line-height:1.25;margin:24px 0 16px;font-weight:600;}"
            "h1{font-size:2em;}h2{font-size:1.5em;}h3{font-size:1.25em;}"
            "h4{font-size:1em;}h5{font-size:.875em;}h6{font-size:.85em;color:var(--muted);}"
            "h1,h2{border-bottom:1px solid var(--border);padding-bottom:.3em;}"
            "p,blockquote,ul,ol,dl,table,pre,details{margin:0 0 16px;}"
            "ul,ol{padding-left:2em;}li+li{margin-top:.25em;}li>p{margin-top:16px;}"
            "a{color:var(--accent);text-decoration:none;}a:hover{text-decoration:underline;}"
            "code,pre,kbd{font-family:'Cascadia Mono','Consolas','Courier New',monospace;}"
            "code{background:var(--surface);border-radius:6px;padding:.2em .4em;font-size:85%;}"
            "pre{background:var(--surface);border-radius:6px;padding:16px;overflow:auto;font-size:85%;line-height:1.45;}"
            "pre code{background:transparent;padding:0;}"
            "blockquote{border-left:.25em solid var(--border);color:var(--muted);margin-left:0;padding:0 1em;}"
            "table{border-collapse:collapse;display:block;width:max-content;max-width:100%;overflow:auto;}"
            "th,td{border:1px solid var(--border);padding:6px 13px;text-align:left;}"
            "th{font-weight:600;}tr:nth-child(even){background:var(--surface);}"
            "img{box-sizing:content-box;max-width:100%;height:auto;}"
            "hr{height:.25em;margin:24px 0;background:var(--border);border:0;}"
            "input[type=checkbox]{accent-color:var(--accent);margin:0 .5em .25em -1.4em;vertical-align:middle;}"
            "kbd{display:inline-block;padding:3px 5px;border:1px solid var(--border);border-radius:6px;"
            "background:var(--surface);box-shadow:inset 0 -1px 0 var(--border);font-size:11px;line-height:10px;}"
            "@media(max-width:640px){body{padding:20px;}}"
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

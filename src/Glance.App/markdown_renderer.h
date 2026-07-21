#pragma once

#include <string>
#include <string_view>

namespace glance::app
{
    [[nodiscard]] std::wstring render_markdown_html(std::wstring_view markdown, bool dark_theme);
}

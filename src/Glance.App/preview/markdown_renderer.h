#pragma once

#include <string>
#include <string_view>
#include "preferences/text_preferences.h"

namespace glance::app
{
    [[nodiscard]] std::wstring render_markdown_html(std::wstring_view markdown, bool dark_theme,
        const TextPreferences& preferences = {});
}

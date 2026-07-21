#pragma once

#include <initializer_list>
#include <string>
#include <string_view>

namespace glance::app
{
    [[nodiscard]] std::wstring resolve_ui_language(std::wstring_view saved_language);
    void apply_ui_language(std::wstring_view language);
    [[nodiscard]] std::wstring localize(std::wstring_view key);
    [[nodiscard]] std::wstring localize_format(
        std::wstring_view key,
        std::initializer_list<std::wstring_view> arguments);
}

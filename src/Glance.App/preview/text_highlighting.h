#pragma once

#include <span>
#include <string_view>
#include <cstdint>

namespace glance::app
{
    enum class HighlightStyle : char
    {
        plain, number, comment, string, preprocessor, error, attribute
    };

    enum class HighlightMatch
    {
        word, digit_pattern, identifier_before
    };

    struct HighlightRule
    {
        HighlightMatch match;
        std::string_view pattern;
        HighlightStyle style;
        bool ignore_case{};
        char allowed_prefix{};
    };

    // Rules are ordered by precedence and match bounded, single-line tokens.
    // '#' matches an ASCII digit in digit_pattern rules; identifier_before matches a field name.
    struct HighlightRuleSet
    {
        std::span<const std::wstring_view> extensions;
        std::span<const HighlightRule> rules;
    };

    inline constexpr std::size_t highlight_context_bytes = 128;
    [[nodiscard]] const HighlightRuleSet* highlighting_for_extension(std::wstring_view extension) noexcept;
    void highlight_text(const HighlightRuleSet& rules, std::string_view text,
        unsigned char preceding, std::span<char> styles) noexcept;
}

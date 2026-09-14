#include "pch.h"
#include "text_highlighting.h"

#include <algorithm>
#include <array>

namespace glance::app
{
    namespace
    {
        constexpr HighlightRule log_rules[] = {
            { HighlightMatch::identifier_before, "=", HighlightStyle::attribute },
            { HighlightMatch::digit_pattern, "####-##-##", HighlightStyle::number },
            { HighlightMatch::digit_pattern, "####/##/##", HighlightStyle::number },
            { HighlightMatch::digit_pattern, "##:##:##.###", HighlightStyle::number, false, 'T' },
            { HighlightMatch::digit_pattern, "##:##:##,###", HighlightStyle::number, false, 'T' },
            { HighlightMatch::digit_pattern, "##:##:##", HighlightStyle::number, false, 'T' },
            { HighlightMatch::word, "TRACE", HighlightStyle::comment, true },
            { HighlightMatch::word, "DEBUG", HighlightStyle::comment, true },
            { HighlightMatch::word, "VERBOSE", HighlightStyle::comment, true },
            { HighlightMatch::word, "DBG", HighlightStyle::comment, true },
            { HighlightMatch::word, "INFO", HighlightStyle::string, true },
            { HighlightMatch::word, "INFORMATION", HighlightStyle::string, true },
            { HighlightMatch::word, "NOTICE", HighlightStyle::string, true },
            { HighlightMatch::word, "INF", HighlightStyle::string, true },
            { HighlightMatch::word, "WARN", HighlightStyle::preprocessor, true },
            { HighlightMatch::word, "WARNING", HighlightStyle::preprocessor, true },
            { HighlightMatch::word, "WRN", HighlightStyle::preprocessor, true },
            { HighlightMatch::word, "ERROR", HighlightStyle::error, true },
            { HighlightMatch::word, "ERR", HighlightStyle::error, true },
            { HighlightMatch::word, "FATAL", HighlightStyle::error, true },
            { HighlightMatch::word, "CRITICAL", HighlightStyle::error, true },
            { HighlightMatch::word, "PANIC", HighlightStyle::error, true },
            { HighlightMatch::word, "FTL", HighlightStyle::error, true },
        };
        constexpr std::wstring_view log_extensions[] = { L".log" };
        constexpr HighlightRuleSet rule_sets[] = { { log_extensions, log_rules } };

        bool word(unsigned char c) noexcept
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c >= 128;
        }
        char upper(char c) noexcept
        {
            return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        }
    }

    const HighlightRuleSet* highlighting_for_extension(std::wstring_view extension) noexcept
    {
        for (const auto& set : rule_sets)
        {
            if (std::ranges::find(set.extensions, extension) != set.extensions.end()) { return &set; }
        }
        return nullptr;
    }

    void highlight_text(const HighlightRuleSet& set, std::string_view text,
        unsigned char preceding, std::span<char> styles) noexcept
    {
        text = text.substr(0, std::min(text.size(), styles.size()));
        std::ranges::fill(styles, static_cast<char>(HighlightStyle::plain));
        for (std::size_t i = 0; i < text.size();)
        {
            const auto before = i ? static_cast<unsigned char>(text[i - 1]) : preceding;
            std::size_t matched{};
            for (const auto& rule : set.rules)
            {
                if (word(before) && (rule.allowed_prefix == 0 || before != rule.allowed_prefix)) { continue; }
                auto length = rule.pattern.size();
                if (length == 0 || length >= highlight_context_bytes) { continue; }
                if (rule.match == HighlightMatch::identifier_before)
                {
                    length = 0;
                    while (i + length < text.size() && word(static_cast<unsigned char>(text[i + length])) &&
                        length < highlight_context_bytes - rule.pattern.size()) { ++length; }
                    if (!length || text.substr(i + length, rule.pattern.size()) != rule.pattern) { continue; }
                }
                else
                {
                    if (i + length > text.size()) { continue; }
                    bool equal = true;
                    for (std::size_t j = 0; j < length; ++j)
                    {
                        const auto c = text[i + j];
                        const auto expected = rule.pattern[j];
                        if (rule.match == HighlightMatch::digit_pattern && expected == '#')
                        {
                            equal = c >= '0' && c <= '9';
                        }
                        else { equal = rule.ignore_case ? upper(c) == upper(expected) : c == expected; }
                        if (!equal) { break; }
                    }
                    if (!equal) { continue; }
                    if (rule.match == HighlightMatch::word && i + length < text.size() &&
                        word(static_cast<unsigned char>(text[i + length]))) { continue; }
                }
                std::fill_n(styles.begin() + i, length, static_cast<char>(rule.style));
                matched = length;
                break;
            }
            i += matched ? matched : 1;
        }
    }
}

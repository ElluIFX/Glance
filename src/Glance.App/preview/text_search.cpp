#include "pch.h"
#include "text_search.h"
#include "json_preview.h"

#include <icu.h>
#include <limits>

namespace glance::app
{
    namespace
    {
        UBool U_CALLCONV keep_matching(const void* context, std::int32_t)
        {
            return !static_cast<const std::atomic_bool*>(context)->load(std::memory_order_relaxed);
        }

        UBool U_CALLCONV keep_finding(const void* context, std::int64_t)
        {
            return keep_matching(context, 0);
        }

        bool word_character(std::string_view text, std::int64_t position, bool before)
        {
            if (position < 0 || (position >= static_cast<std::int64_t>(text.size()) && !before)) return false;
            if (before)
            {
                if (position == 0) return false;
                --position;
                while (position > 0 && (static_cast<unsigned char>(text[static_cast<std::size_t>(position)]) & 0xc0) == 0x80)
                    --position;
            }
            UChar32 character{};
            const auto lead = static_cast<unsigned char>(text[static_cast<std::size_t>(position)]);
            const int length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
            character = lead & (length == 1 ? 0x7f : length == 2 ? 0x1f : length == 3 ? 0x0f : 0x07);
            for (int i = 1; i < length && position + i < static_cast<std::int64_t>(text.size()); ++i)
                character = (character << 6) | (static_cast<unsigned char>(text[static_cast<std::size_t>(position + i)]) & 0x3f);
            const auto type = u_charType(character);
            return character == '_' || u_isalnum(character) || type == U_NON_SPACING_MARK || type == U_COMBINING_SPACING_MARK;
        }
    }

    struct TextSearchMatcher::Impl
    {
        URegularExpression* expression{};
        UErrorCode status{U_ZERO_ERROR};
        UParseError parse{};
        bool whole_word{};
        bool empty{};
        ~Impl() { if (expression) uregex_close(expression); }
    };

    TextSearchMatcher::TextSearchMatcher(std::wstring_view query, TextSearchOptions options)
        : impl_(std::make_unique<Impl>())
    {
        impl_->empty = query.empty();
        impl_->whole_word = options.whole_word;
        if (impl_->empty) return;
        if (query.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
        {
            impl_->status = U_ILLEGAL_ARGUMENT_ERROR;
            return;
        }
        const std::uint32_t flags = UREGEX_MULTILINE | (options.match_case ? 0 : UREGEX_CASE_INSENSITIVE) |
            (options.regular_expression ? 0 : UREGEX_LITERAL);
        impl_->expression = uregex_open(reinterpret_cast<const UChar*>(query.data()),
            static_cast<std::int32_t>(query.size()), flags, &impl_->parse, &impl_->status);
        if (U_SUCCESS(impl_->status))
            uregex_setTimeLimit(impl_->expression, 5000, &impl_->status);
    }

    TextSearchMatcher::~TextSearchMatcher() = default;

    JsonSearchResult search_json_nodes(std::span<const JsonNode> nodes, std::wstring_view query,
        TextSearchOptions options, JsonSearchScope scope, const std::atomic_bool& cancelled)
    {
        TextSearchMatcher matcher(query, options);
        JsonSearchResult result;
        result.error = matcher.find_all("", cancelled).error;
        const auto collect = [&](const JsonNode& node, bool key) {
            const auto text = winrt::to_string(winrt::hstring(key ? node.key : node.value));
            auto found = matcher.find_all(text, cancelled);
            result.error = found.error;
            for (const auto& match : found.matches) result.matches.push_back({match, node.id, key});
        };
        for (const auto& node : nodes)
        {
            if (cancelled.load() || result.error != TextSearchError::none) break;
            if (scope != JsonSearchScope::values && node.parent_id < nodes.size() && nodes[node.parent_id].kind == JsonNodeKind::object)
                collect(node, true);
            if (scope != JsonSearchScope::keys && node.kind >= JsonNodeKind::string && node.kind <= JsonNodeKind::null_value && result.error == TextSearchError::none)
                collect(node, false);
        }
        if (cancelled.load()) result.error = TextSearchError::cancelled;
        if (result.error != TextSearchError::none) result.matches.clear();
        return result;
    }

    TextSearchResult TextSearchMatcher::find_all(std::string_view text, const std::atomic_bool& cancelled)
    {
        TextSearchResult result;
        if (U_FAILURE(impl_->status))
        {
            result.error = TextSearchError::invalid_expression;
            result.error_offset = impl_->parse.offset;
            return result;
        }
        if (impl_->empty) return result;
        UErrorCode status = U_ZERO_ERROR;
        UText input = UTEXT_INITIALIZER;
        utext_openUTF8(&input, text.data(), static_cast<std::int64_t>(text.size()), &status);
        struct InputOwner { UText* text; ~InputOwner() { utext_close(text); } } owner{&input};
        uregex_setUText(impl_->expression, &input, &status);
        uregex_setMatchCallback(impl_->expression, keep_matching, &cancelled, &status);
        uregex_setFindProgressCallback(impl_->expression, keep_finding, &cancelled, &status);
        while (U_SUCCESS(status) && !cancelled.load(std::memory_order_relaxed) &&
            uregex_findNext(impl_->expression, &status))
        {
            const auto start = uregex_start64(impl_->expression, 0, &status);
            const auto end = uregex_end64(impl_->expression, 0, &status);
            if (!impl_->whole_word || (!word_character(text, start, true) && !word_character(text, end, false)))
                result.matches.push_back({start, end});
        }
        if (cancelled.load(std::memory_order_relaxed) || status == U_REGEX_STOPPED_BY_CALLER)
            result.error = TextSearchError::cancelled;
        else if (status == U_REGEX_TIME_OUT || status == U_REGEX_STACK_OVERFLOW)
            result.error = TextSearchError::timed_out;
        else if (U_FAILURE(status)) result.error = TextSearchError::failed;
        if (result.error != TextSearchError::none) result.matches.clear();
        // Detach borrowed text before the caller releases its snapshot.
        status = U_ZERO_ERROR;
        static const UChar empty[] = {0};
        uregex_setText(impl_->expression, empty, 0, &status);
        return result;
    }
}

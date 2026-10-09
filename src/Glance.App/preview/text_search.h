#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace glance::app
{
    struct TextSearchOptions
    {
        bool match_case{};
        bool whole_word{};
        bool regular_expression{};
    };

    struct TextSearchMatch
    {
        std::int64_t start{};
        std::int64_t end{};
    };

    enum class TextSearchError { none, invalid_expression, timed_out, cancelled, failed };

    struct TextSearchResult
    {
        std::vector<TextSearchMatch> matches;
        TextSearchError error{};
        std::int32_t error_offset{};
    };

    struct JsonNode;
    enum class JsonSearchScope { both, keys, values };
    struct JsonSearchMatch
    {
        TextSearchMatch match;
        std::size_t node_id{};
        bool key{};
    };
    struct JsonSearchResult
    {
        std::vector<JsonSearchMatch> matches;
        TextSearchError error{};
    };
    [[nodiscard]] JsonSearchResult search_json_nodes(std::span<const JsonNode> nodes,
        std::wstring_view query, TextSearchOptions options, JsonSearchScope scope, const std::atomic_bool& cancelled);

    class TextSearchMatcher
    {
    public:
        TextSearchMatcher(std::wstring_view query, TextSearchOptions options);
        ~TextSearchMatcher();
        TextSearchMatcher(const TextSearchMatcher&) = delete;
        TextSearchMatcher& operator=(const TextSearchMatcher&) = delete;
        [[nodiscard]] TextSearchResult find_all(std::string_view text, const std::atomic_bool& cancelled);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}

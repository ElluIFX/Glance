#include "text_search.h"
#include "json_preview.h"
#include <future>
#include <chrono>
#include <thread>
#include <iostream>

int run_text_search_tests()
{
    using namespace glance::app;
    int failures{};
    const auto expect = [&](bool condition, const char* label) {
        std::cout << (condition ? "PASS " : "FAIL ") << label << std::endl;
        if (!condition) ++failures;
    };
    std::atomic_bool cancelled{};
    TextSearchMatcher literal(L"a.b", {});
    expect(literal.find_all("A.B a-b", cancelled).matches.size() == 1, "literal search preserves metacharacters and folds case");
    TextSearchMatcher exact(L"a", {.match_case=true});
    expect(exact.find_all("A a", cancelled).matches.size() == 1, "case-sensitive search");
    TextSearchMatcher word(L"word", {.whole_word=true});
    expect(word.find_all("word words _word word!", cancelled).matches.size() == 2, "whole-word search excludes suffixes and identifiers");
    TextSearchMatcher unicode(L"中文", {});
    const auto positions = unicode.find_all("x\xe4\xb8\xad\xe6\x96\x87", cancelled).matches;
    expect(positions.size() == 1 && positions[0].start == 1 && positions[0].end == 7, "Unicode matches use UTF-8 byte positions");
    TextSearchMatcher expression(L"(?<=name: )([A-Z]+)\\n\\1", {.regular_expression=true});
    expect(expression.find_all("name: ABC\nABC", cancelled).matches.size() == 1, "lookbehind, capture, backreference and multiline match");
    TextSearchMatcher empty_match(L"(?=a)", {.regular_expression=true});
    expect(empty_match.find_all("aa", cancelled).matches.size() == 2, "zero-length matching advances safely");
    TextSearchMatcher invalid(L"[", {.regular_expression=true});
    expect(invalid.find_all("text", cancelled).error == TextSearchError::invalid_expression, "invalid expression is reported");
    std::string joined(256 * 1024 - 1, 'x'); joined += "boundary";
    TextSearchMatcher boundary(L"xboundary", {});
    expect(boundary.find_all(joined, cancelled).matches.size() == 1, "matching spans the incremental read boundary");
    cancelled = true;
    expect(boundary.find_all(joined, cancelled).error == TextSearchError::cancelled, "cancelled search publishes no matches");
    cancelled = false;
    expect(literal.find_all("a.b", cancelled).matches.size() == 1, "matcher reuses independent input snapshots");
    const std::vector<JsonNode> nodes{
        {.id=0, .kind=JsonNodeKind::object},
        {.id=1, .parent_id=0, .kind=JsonNodeKind::string, .key=L"target", .value=L"target"},
        {.id=2, .parent_id=0, .kind=JsonNodeKind::array, .key=L"items"},
        {.id=3, .parent_id=2, .kind=JsonNodeKind::string, .key=L"0", .value=L"line\nbreak"},
        {.id=4, .parent_id=0, .kind=JsonNodeKind::boolean, .key=L"enabled", .value=L"true"},
    };
    const auto keys = search_json_nodes(nodes, L"target", {}, JsonSearchScope::keys, cancelled);
    const auto values = search_json_nodes(nodes, L"target", {}, JsonSearchScope::values, cancelled);
    expect(keys.matches.size() == 1 && keys.matches[0].key && values.matches.size() == 1 && !values.matches[0].key, "JSON key and value scopes remain independent");
    expect(search_json_nodes(nodes, L"target", {}, JsonSearchScope::both, cancelled).matches.size() == 2, "JSON both scope counts matching fields");
    expect(search_json_nodes(nodes, L"0", {}, JsonSearchScope::keys, cancelled).matches.empty(), "JSON array indices are not object keys");
    expect(search_json_nodes(nodes, L"line\\nbreak", {.regular_expression=true}, JsonSearchScope::values, cancelled).matches.size() == 1, "JSON searches decoded string values");
    expect(search_json_nodes(nodes, L"true", {}, JsonSearchScope::values, cancelled).matches.size() == 1, "JSON searches scalar primitive values");
    TextSearchMatcher expensive(L"(a+)+$", {.regular_expression=true});
    const std::string difficult = std::string(128 * 1024, 'a') + '!';
    auto running = std::async(std::launch::async, [&] { return expensive.find_all(difficult, cancelled); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    cancelled = true;
    expect(running.wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
        running.get().error == TextSearchError::cancelled, "expensive regular expression observes cancellation");
    return failures ? 1 : 0;
}

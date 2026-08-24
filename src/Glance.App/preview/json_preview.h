#pragma once

#include "preview_provider.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace glance::app
{
    inline constexpr std::size_t json_no_parent =
        static_cast<std::size_t>(-1);

    enum class JsonNodeKind
    {
        pending,
        object,
        array,
        string,
        number,
        boolean,
        null_value,
        error,
    };

    struct JsonNode
    {
        std::size_t id{};
        std::size_t parent_id{ json_no_parent };
        std::uint32_t depth{};
        JsonNodeKind kind{ JsonNodeKind::pending };
        std::wstring key;
        std::wstring value;
        std::uint64_t child_count{};
        bool complete{};
    };

    struct JsonStatistics
    {
        std::uint64_t object_count{};
        std::uint64_t array_count{};
        std::uint64_t value_count{};
        std::uint32_t maximum_depth{};
    };

    struct JsonParseBatch
    {
        std::vector<JsonNode> added_nodes;
        struct NodeUpdate
        {
            std::size_t id{};
            JsonNodeKind kind{ JsonNodeKind::pending };
            std::wstring value;
            std::uint64_t child_count{};
            bool complete{};
            bool replace_kind_and_value{};
        };
        std::vector<NodeUpdate> updated_nodes;
        JsonStatistics statistics;
        std::wstring source_error;
        std::size_t error_line{ 1 };
        std::size_t error_column{ 1 };
        bool complete{};
        bool cancelled{};
        bool fatal_error{};
        bool index_truncated{};
    };

    class JsonPreviewParser final
    {
    public:
        JsonPreviewParser(
            std::wstring path,
            TextEncoding encoding,
            bool json_lines);
        ~JsonPreviewParser();

        JsonPreviewParser(const JsonPreviewParser&) = delete;
        JsonPreviewParser& operator=(const JsonPreviewParser&) = delete;

        [[nodiscard]] JsonParseBatch parse_next_batch();
        void cancel() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}

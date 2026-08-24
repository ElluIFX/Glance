#include "pch.h"
#include "json_preview.h"

#include <rapidjson/encodings.h>
#include <rapidjson/reader.h>

#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace
{
    constexpr std::size_t text_chunk_bytes = 256U * 1024U;
    constexpr std::size_t maximum_batch_tokens = 512;
    constexpr auto maximum_batch_duration = std::chrono::milliseconds(8);
    constexpr std::size_t maximum_index_bytes = 256U * 1024U * 1024U;
    constexpr unsigned parse_flags =
        rapidjson::kParseIterativeFlag |
        rapidjson::kParseNumbersAsStringsFlag |
        rapidjson::kParseValidateEncodingFlag;

    class DecodedTextStream final
    {
    public:
        using Ch = wchar_t;

        DecodedTextStream(
            std::wstring path,
            glance::app::TextEncoding encoding,
            std::shared_ptr<std::atomic_bool> cancellation)
            : path_(std::move(path)),
              encoding_(encoding),
              cancellation_(std::move(cancellation))
        {
        }

        Ch Peek() const
        {
            refill();
            return position_ < content_.size() ? content_[position_] : L'\0';
        }

        Ch Take()
        {
            const auto value = Peek();
            if (value == L'\0')
            {
                return value;
            }
            ++position_;
            ++offset_;
            if (value == L'\n')
            {
                ++line_;
                column_ = 1;
            }
            else
            {
                ++column_;
            }
            return value;
        }

        [[nodiscard]] std::size_t Tell() const noexcept
        {
            return offset_;
        }

        Ch* PutBegin() noexcept { return nullptr; }
        void Put(Ch) noexcept {}
        void Flush() noexcept {}
        std::size_t PutEnd(Ch*) noexcept { return 0; }

        [[nodiscard]] std::size_t line() const noexcept
        {
            return line_;
        }

        [[nodiscard]] std::size_t column() const noexcept
        {
            return column_;
        }

        [[nodiscard]] const std::wstring& error() const noexcept
        {
            return error_;
        }

        [[nodiscard]] bool cancelled() const noexcept
        {
            return cancellation_->load(std::memory_order_acquire);
        }

        void cancel() noexcept
        {
            cancellation_->store(true, std::memory_order_release);
            glance::app::cancel_text_preview_read(reader_);
        }

        void skip_line()
        {
            while (Peek() != L'\0' && Peek() != L'\r' && Peek() != L'\n')
            {
                Take();
            }
            if (Peek() == L'\r')
            {
                Take();
            }
            if (Peek() == L'\n')
            {
                Take();
            }
        }

    private:
        void refill() const
        {
            while (position_ >= content_.size() && !finished_)
            {
                if (cancelled())
                {
                    finished_ = true;
                    return;
                }
                glance::app::TextPreview preview;
                if (!initialized_)
                {
                    initialized_ = true;
                    preview = glance::app::load_text_preview(
                        path_,
                        text_chunk_bytes,
                        encoding_);
                }
                else if (reader_ != nullptr)
                {
                    preview = glance::app::load_next_text_preview_chunk(
                        reader_,
                        text_chunk_bytes);
                }
                else
                {
                    finished_ = true;
                    return;
                }
                if (!preview.error.empty())
                {
                    error_ = std::move(preview.error);
                    reader_.reset();
                    finished_ = true;
                    return;
                }
                content_ = std::move(preview.content);
                position_ = 0;
                reader_ = std::move(preview.reader);
                finished_ = !preview.has_more;
                if (content_.empty() && finished_)
                {
                    return;
                }
            }
        }

        std::wstring path_;
        glance::app::TextEncoding encoding_{};
        std::shared_ptr<std::atomic_bool> cancellation_;
        mutable std::wstring content_;
        mutable std::wstring error_;
        mutable std::shared_ptr<glance::app::IncrementalTextReader> reader_;
        mutable std::size_t position_{};
        mutable std::size_t offset_{};
        mutable std::size_t line_{ 1 };
        mutable std::size_t column_{ 1 };
        mutable bool initialized_{};
        mutable bool finished_{};
    };

    class JsonLineStream final
    {
    public:
        using Ch = wchar_t;

        explicit JsonLineStream(DecodedTextStream& source) noexcept
            : source_(source), start_(source.Tell())
        {
        }

        Ch Peek() const
        {
            const auto value = source_.Peek();
            return value == L'\r' || value == L'\n' ? L'\0' : value;
        }

        Ch Take()
        {
            return Peek() == L'\0' ? L'\0' : source_.Take();
        }

        [[nodiscard]] std::size_t Tell() const noexcept
        {
            return source_.Tell() - start_;
        }

        Ch* PutBegin() noexcept { return nullptr; }
        void Put(Ch) noexcept {}
        void Flush() noexcept {}
        std::size_t PutEnd(Ch*) noexcept { return 0; }

    private:
        DecodedTextStream& source_;
        std::size_t start_{};
    };
}

namespace glance::app
{
    struct JsonPreviewParser::Impl
    {
        using Reader = rapidjson::GenericReader<
            rapidjson::UTF16<wchar_t>,
            rapidjson::UTF16<wchar_t>>;

        struct Frame
        {
            std::optional<std::size_t> node_id;
            JsonNodeKind kind{};
            std::uint64_t next_index{};
            std::uint64_t child_count{};
        };

        struct Handler
        {
            explicit Handler(Impl& owner) noexcept : owner_(owner) {}

            bool Null() { return owner_.scalar(JsonNodeKind::null_value, L"null"); }
            bool Bool(bool value)
            {
                return owner_.scalar(
                    JsonNodeKind::boolean,
                    value ? L"true" : L"false");
            }
            bool Int(int value) { return number(std::to_wstring(value)); }
            bool Uint(unsigned value) { return number(std::to_wstring(value)); }
            bool Int64(std::int64_t value) { return number(std::to_wstring(value)); }
            bool Uint64(std::uint64_t value) { return number(std::to_wstring(value)); }
            bool Double(double value)
            {
                return number(std::to_wstring(value));
            }
            bool RawNumber(const wchar_t* value, rapidjson::SizeType length, bool)
            {
                return owner_.scalar(
                    JsonNodeKind::number,
                    std::wstring(value, length));
            }
            bool String(const wchar_t* value, rapidjson::SizeType length, bool)
            {
                return owner_.scalar(
                    JsonNodeKind::string,
                    std::wstring(value, length));
            }
            bool StartObject() { return owner_.start_container(JsonNodeKind::object); }
            bool Key(const wchar_t* value, rapidjson::SizeType length, bool)
            {
                owner_.pending_key_.assign(value, length);
                return true;
            }
            bool EndObject(rapidjson::SizeType) { return owner_.end_container(); }
            bool StartArray() { return owner_.start_container(JsonNodeKind::array); }
            bool EndArray(rapidjson::SizeType) { return owner_.end_container(); }

        private:
            bool number(std::wstring value)
            {
                return owner_.scalar(JsonNodeKind::number, std::move(value));
            }

            Impl& owner_;
        };

        Impl(std::wstring path, TextEncoding encoding, bool json_lines)
            : cancellation_(std::make_shared<std::atomic_bool>(false)),
              source_(std::move(path), encoding, cancellation_),
              json_lines_(json_lines),
              handler_(*this)
        {
        }

        JsonParseBatch parse_next_batch()
        {
            batch_ = {};
            if (complete_ || cancelled_ || fatal_error_)
            {
                copy_state_to_batch();
                return std::move(batch_);
            }

            if (json_lines_ && !synthetic_root_added_)
            {
                synthetic_root_added_ = true;
                add_node(json_no_parent, 0, JsonNodeKind::array, {}, {});
            }

            const auto deadline = std::chrono::steady_clock::now() +
                maximum_batch_duration;
            std::size_t tokens{};
            while (tokens < maximum_batch_tokens &&
                   std::chrono::steady_clock::now() < deadline &&
                   !complete_ && !cancelled_ && !fatal_error_)
            {
                if (source_.cancelled())
                {
                    cancelled_ = true;
                    break;
                }
                if (reader_ == nullptr)
                {
                    if (json_lines_)
                    {
                        if (!begin_json_line())
                        {
                            complete_ = source_.error().empty();
                            fatal_error_ = !source_.error().empty();
                            source_error_ = source_.error();
                            break;
                        }
                    }
                    else
                    {
                        reader_ = std::make_unique<Reader>();
                        reader_->IterativeParseInit();
                    }
                }

                const bool parsed = json_lines_
                    ? reader_->IterativeParseNext<parse_flags>(*line_stream_, handler_)
                    : reader_->IterativeParseNext<parse_flags>(source_, handler_);
                ++tokens;
                if (!parsed)
                {
                    if (source_.cancelled())
                    {
                        cancelled_ = true;
                    }
                    else if (json_lines_)
                    {
                        fail_json_line();
                    }
                    else
                    {
                        fail_document();
                    }
                    continue;
                }
                if (reader_->IterativeParseComplete())
                {
                    if (json_lines_)
                    {
                        finish_json_line();
                    }
                    else
                    {
                        complete_ = true;
                    }
                }
            }
            copy_state_to_batch();
            release_delivered_text();
            return std::move(batch_);
        }

        void cancel() noexcept
        {
            source_.cancel();
        }

        bool scalar(JsonNodeKind kind, std::wstring value)
        {
            ++statistics_.value_count;
            statistics_.maximum_depth = std::max(
                statistics_.maximum_depth,
                current_value_depth());
            const auto node_id = begin_value(kind, std::move(value), true);
            if (node_id.has_value())
            {
                update_node(*node_id, [](JsonNode& node) { node.complete = true; });
            }
            return true;
        }

        bool start_container(JsonNodeKind kind)
        {
            if (kind == JsonNodeKind::object)
            {
                ++statistics_.object_count;
            }
            else
            {
                ++statistics_.array_count;
            }
            statistics_.maximum_depth = std::max(
                statistics_.maximum_depth,
                current_value_depth());
            auto node_id = begin_value(kind, {}, false);
            frames_.push_back({ node_id, kind, 0, 0 });
            return true;
        }

        [[nodiscard]] std::uint32_t current_value_depth() const noexcept
        {
            if (json_lines_ && frames_.empty() && record_node_id_.has_value())
            {
                return 1;
            }
            return static_cast<std::uint32_t>(
                frames_.size() + (json_lines_ ? 1U : 0U));
        }

        bool end_container()
        {
            if (frames_.empty())
            {
                return false;
            }
            auto frame = frames_.back();
            frames_.pop_back();
            if (frame.node_id.has_value())
            {
                update_node(*frame.node_id, [frame](JsonNode& node) {
                    node.child_count = frame.child_count;
                    node.complete = true;
                });
            }
            return true;
        }

        std::optional<std::size_t> begin_value(
            JsonNodeKind kind,
            std::wstring value,
            bool complete)
        {
            std::size_t parent_id = json_no_parent;
            std::uint32_t depth{};
            std::wstring key;
            if (!frames_.empty())
            {
                auto& parent = frames_.back();
                parent_id = parent.node_id.value_or(json_no_parent);
                depth = static_cast<std::uint32_t>(
                    frames_.size() + (json_lines_ ? 1U : 0U));
                key = parent.kind == JsonNodeKind::array
                    ? std::to_wstring(parent.next_index++)
                    : std::exchange(pending_key_, {});
                ++parent.child_count;
            }

            if (json_lines_ && frames_.empty() && record_node_id_.has_value())
            {
                const auto node_id = *record_node_id_;
                update_node(node_id, [kind, value = std::move(value), complete](JsonNode& node) mutable {
                    node.kind = kind;
                    node.value = std::move(value);
                    node.complete = complete;
                }, true);
                record_root_started_ = true;
                return node_id;
            }
            return add_node(
                parent_id,
                depth,
                kind,
                std::move(key),
                std::move(value),
                complete);
        }

        std::optional<std::size_t> add_node(
            std::size_t parent_id,
            std::uint32_t depth,
            JsonNodeKind kind,
            std::wstring key,
            std::wstring value,
            bool complete = false)
        {
            const auto estimated_bytes = sizeof(JsonNode) * 2 +
                (key.size() + value.size()) * sizeof(wchar_t);
            if (index_truncated_ || indexed_bytes_ + estimated_bytes > maximum_index_bytes)
            {
                index_truncated_ = true;
                return std::nullopt;
            }
            const auto id = nodes_.size();
            JsonNode node{
                .id = id,
                .parent_id = parent_id,
                .depth = depth,
                .kind = kind,
                .key = std::move(key),
                .value = std::move(value),
                .complete = complete };
            indexed_bytes_ += estimated_bytes;
            nodes_.push_back(std::move(node));
            batch_.added_nodes.push_back(nodes_.back());
            return id;
        }

        template <typename Callback>
        void update_node(
            std::size_t id,
            Callback&& callback,
            bool replace_kind_and_value = false)
        {
            if (id >= nodes_.size())
            {
                return;
            }
            callback(nodes_[id]);
            const auto& node = nodes_[id];
            batch_.updated_nodes.push_back({
                .id = node.id,
                .kind = node.kind,
                .value = replace_kind_and_value ? node.value : std::wstring{},
                .child_count = node.child_count,
                .complete = node.complete,
                .replace_kind_and_value = replace_kind_and_value });
        }

        bool begin_json_line()
        {
            while (true)
            {
                while (source_.Peek() == L' ' || source_.Peek() == L'\t')
                {
                    source_.Take();
                }
                if (source_.Peek() == L'\r' || source_.Peek() == L'\n')
                {
                    source_.skip_line();
                    continue;
                }
                break;
            }
            if (source_.Peek() == L'\0')
            {
                return false;
            }

            record_line_ = source_.line();
            record_statistics_ = statistics_;
            ++record_count_;
            auto& root = nodes_.front();
            root.child_count = record_count_;
            batch_.updated_nodes.push_back({
                .id = root.id,
                .kind = root.kind,
                .child_count = root.child_count,
                .complete = root.complete });
            record_node_id_ = add_node(
                root.id,
                1,
                JsonNodeKind::pending,
                std::to_wstring(record_count_ - 1),
                {},
                false);
            record_root_started_ = false;
            pending_key_.clear();
            frames_.clear();
            line_stream_ = std::make_unique<JsonLineStream>(source_);
            reader_ = std::make_unique<Reader>();
            reader_->IterativeParseInit();
            return true;
        }

        void finish_json_line()
        {
            if (record_node_id_.has_value() && !record_root_started_)
            {
                update_node(*record_node_id_, [](JsonNode& node) {
                    node.kind = JsonNodeKind::error;
                    node.complete = true;
                }, true);
            }
            source_.skip_line();
            reset_json_line();
        }

        void fail_json_line()
        {
            statistics_ = record_statistics_;
            if (record_node_id_.has_value())
            {
                const auto line = record_line_;
                update_node(*record_node_id_, [line](JsonNode& node) {
                    node.kind = JsonNodeKind::error;
                    node.value = std::to_wstring(line);
                    node.complete = true;
                }, true);
            }
            source_.skip_line();
            reset_json_line();
        }

        void reset_json_line()
        {
            reader_.reset();
            line_stream_.reset();
            frames_.clear();
            pending_key_.clear();
            record_node_id_.reset();
            record_root_started_ = false;
        }

        void fail_document()
        {
            fatal_error_ = true;
            source_error_ = source_.error();
            error_line_ = source_.line();
            error_column_ = source_.column();
        }

        void copy_state_to_batch()
        {
            for (const auto& frame : frames_)
            {
                if (frame.node_id.has_value())
                {
                    auto& node = nodes_[*frame.node_id];
                    node.child_count = frame.child_count;
                    batch_.updated_nodes.push_back({
                        .id = node.id,
                        .kind = node.kind,
                        .child_count = node.child_count,
                        .complete = node.complete });
                }
            }
            batch_.source_error = source_error_;
            batch_.statistics = statistics_;
            batch_.error_line = error_line_;
            batch_.error_column = error_column_;
            batch_.complete = complete_;
            batch_.cancelled = cancelled_;
            batch_.fatal_error = fatal_error_;
            batch_.index_truncated = index_truncated_;
        }

        void release_delivered_text()
        {
            for (const auto& node : batch_.added_nodes)
            {
                if (node.id < nodes_.size())
                {
                    std::wstring{}.swap(nodes_[node.id].key);
                    std::wstring{}.swap(nodes_[node.id].value);
                }
            }
        }

        std::shared_ptr<std::atomic_bool> cancellation_;
        DecodedTextStream source_;
        bool json_lines_{};
        Handler handler_;
        std::unique_ptr<Reader> reader_;
        std::unique_ptr<JsonLineStream> line_stream_;
        std::vector<Frame> frames_;
        std::vector<JsonNode> nodes_;
        JsonParseBatch batch_;
        JsonStatistics statistics_;
        JsonStatistics record_statistics_;
        std::wstring pending_key_;
        std::wstring source_error_;
        std::optional<std::size_t> record_node_id_;
        std::size_t record_line_{ 1 };
        std::size_t error_line_{ 1 };
        std::size_t error_column_{ 1 };
        std::size_t indexed_bytes_{};
        std::uint64_t record_count_{};
        bool synthetic_root_added_{};
        bool record_root_started_{};
        bool complete_{};
        bool cancelled_{};
        bool fatal_error_{};
        bool index_truncated_{};
    };

    JsonPreviewParser::JsonPreviewParser(
        std::wstring path,
        TextEncoding encoding,
        bool json_lines)
        : impl_(std::make_unique<Impl>(
              std::move(path),
              encoding,
              json_lines))
    {
    }

    JsonPreviewParser::~JsonPreviewParser() = default;

    JsonParseBatch JsonPreviewParser::parse_next_batch()
    {
        return impl_->parse_next_batch();
    }

    void JsonPreviewParser::cancel() noexcept
    {
        impl_->cancel();
    }
}

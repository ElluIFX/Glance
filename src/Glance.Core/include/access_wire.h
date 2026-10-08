#pragma once

#include "explorer_selection.h"
#include "glance/contracts/access_protocol.h"
#include <span>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <type_traits>
#include <string_view>

namespace glance::core::access_wire
{
    class Writer
    {
    public:
        template<class T> void scalar(T value)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            append(&value, sizeof(value));
        }
        void boolean(bool value) { scalar<std::uint8_t>(value ? 1 : 0); }
        void text(const std::wstring& value)
        {
            if (value.size() > 32768) throw std::length_error("Access text limit");
            scalar(static_cast<std::uint32_t>(value.size()));
            append(value.data(), value.size() * sizeof(wchar_t));
        }
        void bytes(const std::vector<std::uint8_t>& value)
        {
            scalar(static_cast<std::uint32_t>(value.size()));
            append(value.data(), value.size());
        }
        std::string data;
    private:
        void append(const void* value, std::size_t size)
        {
            if (size > contracts::access::maximum_payload - data.size()) throw std::length_error("Access payload limit");
            if (size) data.append(static_cast<const char*>(value), size);
        }
    };

    class Reader
    {
    public:
        explicit Reader(std::string_view data) : remaining_(data) {}
        template<class T> T scalar()
        {
            T value{};
            static_assert(std::is_trivially_copyable_v<T>);
            const auto bytes = take(sizeof(value));
            std::memcpy(&value, bytes.data(), sizeof(value));
            return value;
        }
        bool boolean()
        {
            const auto value = scalar<std::uint8_t>();
            if (value > 1) throw std::runtime_error("Invalid access Boolean");
            return value != 0;
        }
        std::wstring text()
        {
            const auto length = scalar<std::uint32_t>();
            if (length > 32768) throw std::runtime_error("Invalid access text length");
            const auto bytes = take(static_cast<std::size_t>(length) * sizeof(wchar_t));
            std::wstring value(length, L'\0');
            std::memcpy(value.data(), bytes.data(), bytes.size());
            return value;
        }
        std::vector<std::uint8_t> bytes()
        {
            const auto value = take(scalar<std::uint32_t>());
            return {value.begin(), value.end()};
        }
        void finish() const { if (!remaining_.empty()) throw std::runtime_error("Trailing access payload"); }
    private:
        std::string_view take(std::size_t size)
        {
            if (size > remaining_.size()) throw std::runtime_error("Truncated access payload");
            const auto value = remaining_.substr(0, size);
            remaining_.remove_prefix(size);
            return value;
        }
        std::string_view remaining_;
    };

    inline void write_file(Writer& out, const contracts::FileDescriptor& file)
    {
        out.text(file.display_name); out.text(file.filesystem_path); out.text(file.shell_parsing_name);
        out.bytes(file.shell_id_list); out.scalar(file.size); out.scalar(file.creation_time);
        out.scalar(file.last_write_time); out.scalar(file.attributes); out.boolean(file.is_filesystem);
        out.boolean(file.is_cloud_placeholder); out.boolean(file.is_hydrated);
    }

    inline contracts::FileDescriptor read_file(Reader& in)
    {
        contracts::FileDescriptor file;
        file.display_name = in.text(); file.filesystem_path = in.text(); file.shell_parsing_name = in.text();
        file.shell_id_list = in.bytes(); file.size = in.scalar<std::uint64_t>();
        file.creation_time = in.scalar<std::uint64_t>(); file.last_write_time = in.scalar<std::uint64_t>();
        file.attributes = in.scalar<std::uint32_t>(); file.is_filesystem = in.boolean();
        file.is_cloud_placeholder = in.boolean(); file.is_hydrated = in.boolean();
        return file;
    }

    inline std::string selection(const contracts::SelectionSnapshot& snapshot, bool suppressed)
    {
        Writer out;
        out.boolean(suppressed); out.scalar(snapshot.timestamp_ms); out.scalar(snapshot.source_window);
        out.scalar(snapshot.source_process_id); out.scalar(snapshot.host_kind); out.text(snapshot.source_id);
        out.scalar(snapshot.source_capabilities); out.boolean(snapshot.accepts_hotkey); out.boolean(snapshot.text_input_active);
        out.scalar(snapshot.focused_index);
        const auto count = static_cast<std::uint32_t>(snapshot.items.size());
        out.scalar(count);
        for (std::uint32_t i = 0; i < count; ++i) write_file(out, snapshot.items[i]);
        return std::move(out.data);
    }

    inline contracts::SelectionSnapshot selection(std::string_view payload, bool& suppressed)
    {
        Reader in(payload);
        contracts::SelectionSnapshot snapshot;
        suppressed = in.boolean(); snapshot.timestamp_ms = in.scalar<std::uint64_t>();
        snapshot.source_window = in.scalar<std::uintptr_t>(); snapshot.source_process_id = in.scalar<std::uint32_t>();
        snapshot.host_kind = in.scalar<contracts::HostKind>(); snapshot.source_id = in.text();
        if (snapshot.host_kind > contracts::HostKind::external_source) throw std::runtime_error("Invalid access host kind");
        snapshot.source_capabilities = in.scalar<std::uint64_t>(); snapshot.accepts_hotkey = in.boolean();
        snapshot.text_input_active = in.boolean(); snapshot.focused_index = in.scalar<std::uint32_t>();
        const auto count = in.scalar<std::uint32_t>();
        if (count > contracts::access::maximum_payload) throw std::runtime_error("Invalid access selection count");
        for (std::uint32_t i = 0; i < count; ++i) snapshot.items.push_back(read_file(in));
        if (count && snapshot.focused_index >= count) snapshot.focused_index = 0;
        in.finish();
        return snapshot;
    }

    inline std::string gallery(const GalleryCommand& command)
    {
        Writer out;
        out.scalar(command.operation); out.scalar(command.window_id); out.scalar(command.request_id);
        out.scalar(command.session_id); out.scalar(command.source_window); out.text(command.source_id);
        out.scalar(command.page_start); out.scalar(command.page_count); out.scalar(command.target_index);
        out.scalar(command.navigation_steps); out.boolean(command.loop); out.text(command.current_path);
        out.scalar(static_cast<std::uint32_t>(command.extensions.size()));
        for (const auto& extension : command.extensions) out.text(extension);
        return std::move(out.data);
    }

    inline GalleryCommand gallery_command(std::string_view payload)
    {
        Reader in(payload);
        GalleryCommand command;
        command.operation = in.scalar<GalleryOperation>();
        if (command.operation > GalleryOperation::close) throw std::runtime_error("Invalid gallery operation");
        command.window_id = in.scalar<std::uint64_t>(); command.request_id = in.scalar<std::uint64_t>();
        command.session_id = in.scalar<std::uint64_t>(); command.source_window = in.scalar<std::uintptr_t>();
        command.source_id = in.text(); command.page_start = in.scalar<std::uint32_t>();
        command.page_count = in.scalar<std::uint32_t>(); command.target_index = in.scalar<std::uint32_t>();
        command.navigation_steps = in.scalar<int>(); command.loop = in.boolean(); command.current_path = in.text();
        const auto count = in.scalar<std::uint32_t>();
        if (count > 1024 || command.page_count > 512) throw std::runtime_error("Invalid gallery limits");
        for (std::uint32_t i = 0; i < count; ++i) command.extensions.push_back(in.text());
        in.finish(); return command;
    }

    inline std::string gallery(const GalleryResponse& response)
    {
        Writer out;
        out.scalar(response.operation); out.scalar(response.window_id); out.scalar(response.request_id);
        out.scalar(response.session_id); out.boolean(response.success); out.text(response.error);
        out.scalar(response.total_count); out.scalar(response.current_index); out.scalar(response.page_start);
        out.boolean(response.total_known); out.scalar(static_cast<std::uint32_t>(response.items.size()));
        for (std::size_t i = 0; i < response.items.size(); ++i)
        {
            write_file(out, response.items[i]);
            out.scalar(i < response.item_indices.size() ? response.item_indices[i] : response.page_start + static_cast<std::uint32_t>(i));
        }
        return std::move(out.data);
    }

    inline GalleryResponse gallery_response(std::string_view payload)
    {
        Reader in(payload);
        GalleryResponse response;
        response.operation = in.scalar<GalleryOperation>(); response.window_id = in.scalar<std::uint64_t>();
        response.request_id = in.scalar<std::uint64_t>(); response.session_id = in.scalar<std::uint64_t>();
        response.success = in.boolean(); response.error = in.text(); response.total_count = in.scalar<std::uint32_t>();
        response.current_index = in.scalar<std::uint32_t>(); response.page_start = in.scalar<std::uint32_t>();
        response.total_known = in.boolean(); const auto count = in.scalar<std::uint32_t>();
        if (count > 512) throw std::runtime_error("Invalid gallery response count");
        for (std::uint32_t i = 0; i < count; ++i)
        {
            response.items.push_back(read_file(in)); response.item_indices.push_back(in.scalar<std::uint32_t>());
        }
        in.finish(); return response;
    }
}

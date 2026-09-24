#pragma once
#include "host/pe_reader.h"
#include <cstring>
#include <stdexcept>
#include <span>

namespace glance::executable
{
inline constexpr std::size_t transfer_capacity = 32 * 1024 * 1024;
struct Query
{
    wchar_t path[32768]{};
    bool detailed{};
};
struct Transfer
{
    Query query;
    std::uint32_t bytes{};
    std::byte data[transfer_capacity];
};
struct Result
{
    Summary summary;
    Table table;
    std::vector<std::pair<std::wstring, Table>> details;
};
class Writer
{
    std::vector<std::byte> bytes_;
  public:
    void raw(const void* value, std::size_t length)
    {
        if (length > transfer_capacity - bytes_.size()) throw std::runtime_error("Partial");
        const auto first = static_cast<const std::byte*>(value);
        bytes_.insert(bytes_.end(), first, first + length);
    }
    template<class T> void number(T value) { raw(&value, sizeof(value)); }
    void text(const std::wstring& value)
    {
        number(static_cast<std::uint32_t>(value.size()));
        raw(value.data(), value.size() * sizeof(wchar_t));
    }
    void table(const Table& value)
    {
        text(value.state); text(value.detail); number(value.field_keys);
        number(static_cast<std::uint32_t>(value.columns.size()));
        for (const auto& column : value.columns) text(column);
        number(static_cast<std::uint32_t>(value.rows.size()));
        for (const auto& row : value.rows)
        {
            number(row.offset); number(row.length); number(row.type); number(row.id); number(row.language);
            number(static_cast<std::uint32_t>(row.cells.size()));
            for (const auto& cell : row.cells) text(cell);
        }
    }
    void result(const Result& value, Transfer& output)
    {
        number(value.summary.identity); text(value.summary.title); text(value.summary.version);
        text(value.summary.architecture); text(value.summary.type);
        number(value.summary.managed); number(value.summary.dll);
        const auto icon_size = value.summary.icon.size() <= 1024 * 1024 ? value.summary.icon.size() : 0;
        number(static_cast<std::uint32_t>(icon_size));
        if (icon_size) raw(value.summary.icon.data(), icon_size);
        table(value.summary.overview); table(value.table);
        number(static_cast<std::uint32_t>(value.details.size()));
        for (const auto& [name, entries] : value.details) { text(name); table(entries); }
        memcpy(output.data, bytes_.data(), bytes_.size());
        output.bytes = static_cast<std::uint32_t>(bytes_.size());
    }
};
class Reader
{
    std::span<const std::byte> bytes_;
  public:
    explicit Reader(const Transfer& transfer)
    {
        if (!transfer.bytes || transfer.bytes > transfer_capacity) throw std::runtime_error("Error");
        bytes_ = {transfer.data, transfer.bytes};
    }
    template<class T> T number()
    {
        if (bytes_.size() < sizeof(T)) throw std::runtime_error("Error");
        T result{}; memcpy(&result, bytes_.data(), sizeof(T)); bytes_ = bytes_.subspan(sizeof(T)); return result;
    }
    std::wstring text()
    {
        const auto count = number<std::uint32_t>();
        if (count > bytes_.size() / sizeof(wchar_t)) throw std::runtime_error("Error");
        std::wstring result(count, L'\0'); memcpy(result.data(), bytes_.data(), count * sizeof(wchar_t));
        bytes_ = bytes_.subspan(count * sizeof(wchar_t)); return result;
    }
    Table table()
    {
        Table value; value.state = text(); value.detail = text(); value.field_keys = number<bool>();
        const auto columns = number<std::uint32_t>();
        if (columns > 64) throw std::runtime_error("Error");
        for (unsigned i = 0; i < columns; ++i) value.columns.push_back(text());
        const auto rows = number<std::uint32_t>();
        if (rows > 100000) throw std::runtime_error("Error");
        for (unsigned i = 0; i < rows; ++i)
        {
            Row row; row.offset = number<std::uint64_t>(); row.length = number<std::uint32_t>();
            row.type = number<std::uint32_t>(); row.id = number<std::uint32_t>(); row.language = number<std::uint16_t>();
            const auto cells = number<std::uint32_t>();
            if (cells > 64) throw std::runtime_error("Error");
            for (unsigned j = 0; j < cells; ++j) row.cells.push_back(text());
            value.rows.push_back(std::move(row));
        }
        return value;
    }
    Result result()
    {
        Result value; value.summary.identity = number<Identity>();
        value.summary.title = text(); value.summary.version = text(); value.summary.architecture = text();
        value.summary.type = text(); value.summary.managed = number<bool>(); value.summary.dll = number<bool>();
        const auto icon_size = number<std::uint32_t>();
        if (icon_size > 1024 * 1024 || icon_size > bytes_.size()) throw std::runtime_error("Error");
        value.summary.icon.assign(bytes_.begin(), bytes_.begin() + icon_size); bytes_ = bytes_.subspan(icon_size);
        value.summary.overview = table(); value.table = table();
        const auto sections = number<std::uint32_t>();
        if (sections > 16) throw std::runtime_error("Error");
        for (unsigned i = 0; i < sections; ++i)
        {
            auto name = text(); auto entries = table();
            value.details.emplace_back(std::move(name), std::move(entries));
        }
        if (!bytes_.empty()) throw std::runtime_error("Error");
        return value;
    }
};
}

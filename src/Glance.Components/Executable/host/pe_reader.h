#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace glance::executable
{
    enum class Section : unsigned
    {
        overview,
        signatures,
        imports,
        exports,
        resources,
        managed,
        structure,
        managed_types,
        managed_methods,
        managed_fields,
        count
    };
    inline constexpr std::array section_keys{L"Overview",  L"Signatures", L"Imports",  L"Exports",
                                             L"Resources", L"Managed", L"Structure", L"ManagedTypes", L"ManagedMethods", L"ManagedFields"};
    struct Row
    {
        std::vector<std::wstring> cells;
        std::uint64_t offset{};
        std::uint32_t length{};
        std::uint32_t type{};
        std::uint32_t id{};
        std::uint16_t language{};
    };
    struct Table
    {
        std::vector<std::wstring> columns;
        std::vector<Row> rows;
        std::wstring state{L"Complete"};
        std::wstring detail;
        bool field_keys{};
        std::size_t text_bytes{};
    };
    struct Identity
    {
        std::uint64_t size{};
        std::uint64_t modified{};
        std::uint64_t index{};
        DWORD volume{};
        bool operator==(const Identity&) const = default;
    };
    struct Summary
    {
        Identity identity;
        std::wstring title;
        std::wstring version;
        std::wstring architecture;
        std::wstring type;
        bool managed{};
        bool dll{};
        Table overview;
        std::vector<std::byte> icon;
    };
    using Cancelled = std::function<bool()>;
    Summary read_summary(const std::wstring& path, const Cancelled& cancelled);
    Table read_section(const std::wstring& path, const Identity& identity, Section section,
                       const Cancelled& cancelled);
    std::vector<std::byte> read_resource(const std::wstring& path, const Identity& identity, const Row& row,
                                         std::size_t limit);
    std::vector<std::byte> icon_file(const std::wstring& path, const Identity& identity, const Row& group);
    std::wstring sha256(const std::wstring& path, const Identity& identity, const Cancelled& cancelled,
                        const std::function<void(unsigned)>& progress = {});
    void export_resource(const std::wstring& path, const Identity& identity, const Row& row,
                         const std::wstring& destination, const Cancelled& cancelled);
    std::wstring hex(std::uint64_t value);
    std::wstring decode_text(const std::vector<std::byte>& bytes);
    void verify_identity(const std::wstring& path, const Identity& identity);
} // namespace glance::executable

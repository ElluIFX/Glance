#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace glance::contracts::sources
{
    inline constexpr std::uint32_t abi_version = 1;
    inline constexpr char get_api_export[] = "GlanceSourceGetApi";
    inline constexpr std::size_t source_id_capacity = 64;
    inline constexpr std::size_t target_app_version_capacity = 32;
    inline constexpr std::size_t display_name_capacity = 128;
    inline constexpr std::size_t status_detail_capacity = 256;
    inline constexpr std::size_t path_capacity = 32768;
    inline constexpr std::uint32_t item_list_api_version = 1;
    inline constexpr std::uint32_t focus_change_api_version = 1;

    inline constexpr GUID item_list_api_id{
        0xf6819698,
        0x099f,
        0x4dd1,
        { 0xb7, 0xea, 0x93, 0x2d, 0x33, 0x16, 0x86, 0x2e } };
    inline constexpr GUID focus_change_api_id{
        0xe3da2778,
        0xc76d,
        0x4ab2,
        { 0x93, 0x62, 0x3d, 0x1f, 0x9f, 0xc1, 0x44, 0x91 } };

    enum class Capability : std::uint64_t
    {
        selection = 1ULL << 0,
        item_list = 1ULL << 1,
        focus_change = 1ULL << 2,
    };

    enum class HealthSeverity : std::uint32_t
    {
        healthy = 0,
        warning = 1,
        error = 2,
    };

    struct SourceHostContext
    {
        std::uint32_t size{ sizeof(SourceHostContext) };
        std::uintptr_t root_window{};
        std::uintptr_t focused_window{};
        std::uintptr_t caret_window{};
        std::uint32_t process_id{};
        std::uint32_t thread_id{};
        std::uint32_t gui_thread_flags{};
        const wchar_t* process_name{};
        const wchar_t* window_class{};
    };

    struct SourceRegistration
    {
        std::uint32_t size{ sizeof(SourceRegistration) };
        wchar_t source_id[source_id_capacity]{};
        wchar_t target_app_version[target_app_version_capacity]{};
        std::uint64_t capability_mask{};
    };

    struct SourceSelectionResult
    {
        std::uint32_t size{ sizeof(SourceSelectionResult) };
        BOOL accepts_hotkey{};
        BOOL text_input_active{};
        std::uint64_t capability_mask{};
        wchar_t filesystem_path[path_capacity]{};
    };

    struct SourceStatusResult
    {
        std::uint32_t size{ sizeof(SourceStatusResult) };
        HealthSeverity severity{ HealthSeverity::error };
        std::uint32_t code{};
        std::uint64_t capability_mask{};
        wchar_t display_name[display_name_capacity]{};
        wchar_t detail[status_detail_capacity]{};
    };

    struct SourceItem
    {
        std::uint64_t item_id{};
        const wchar_t* filesystem_path{};
    };

    using AppendSourceItemFunction = BOOL(WINAPI*)(
        void* context,
        const SourceItem* item) noexcept;

    struct SourceItemSink
    {
        std::uint32_t size{ sizeof(SourceItemSink) };
        void* context{};
        AppendSourceItemFunction append{};
    };

    using InitializeFunction = BOOL(WINAPI*)(SourceRegistration* registration) noexcept;
    using QuerySelectionFunction = BOOL(WINAPI*)(
        const SourceHostContext* context,
        SourceSelectionResult* result) noexcept;
    using QueryStatusFunction = BOOL(WINAPI*)(
        const wchar_t* language_tag,
        SourceStatusResult* result) noexcept;
    using QueryInterfaceFunction = BOOL(WINAPI*)(
        const GUID* interface_id,
        std::uint32_t minimum_version,
        void** interface_pointer) noexcept;
    using ShutdownFunction = void(WINAPI*)() noexcept;

    using QueryItemCountFunction = BOOL(WINAPI*)(
        const SourceHostContext* context,
        std::uint32_t* item_count,
        std::uint32_t* focused_offset,
        std::uint64_t* focused_item_id) noexcept;
    using EnumerateItemsFunction = BOOL(WINAPI*)(
        const SourceHostContext* context,
        std::uint32_t offset,
        std::uint32_t limit,
        const SourceItemSink* sink) noexcept;
    using FocusItemFunction = BOOL(WINAPI*)(
        const SourceHostContext* context,
        std::uint64_t item_id,
        const wchar_t* expected_path) noexcept;

    struct ItemListApi
    {
        std::uint32_t size{ sizeof(ItemListApi) };
        std::uint32_t version{ item_list_api_version };
        QueryItemCountFunction query_count{};
        EnumerateItemsFunction enumerate{};
    };

    struct FocusChangeApi
    {
        std::uint32_t size{ sizeof(FocusChangeApi) };
        std::uint32_t version{ focus_change_api_version };
        FocusItemFunction focus{};
    };

    struct SourceApi
    {
        std::uint32_t size{ sizeof(SourceApi) };
        std::uint32_t abi{ abi_version };
        InitializeFunction initialize{};
        QuerySelectionFunction query_selection{};
        QueryStatusFunction query_status{};
        QueryInterfaceFunction query_interface{};
        ShutdownFunction shutdown{};
    };

    using GetApiFunction = BOOL(WINAPI*)(
        std::uint32_t host_abi,
        SourceApi* api) noexcept;
}

#pragma once

#include <windows.h>
#include <cstdint>

namespace glance::contracts::dependencies
{
    inline constexpr std::uint32_t host_api_version = 1;
    inline constexpr GUID host_api_id{0xbb13a0b7, 0x6432, 0x4194, {0xa8, 0xcc, 0xe2, 0x18, 0x47, 0x64, 0x19, 0x09}};

    enum class EntryKind : std::uint32_t { executable, library };
    enum class Availability : std::uint32_t { missing, managed, external, downloading, installing, failed, in_use };

    struct File
    {
        const wchar_t* archive_path{};
        const wchar_t* installed_path{};
        const wchar_t* sha256{};
    };
    struct Entry
    {
        const wchar_t* id{};
        EntryKind kind{};
        const wchar_t* installed_path{};
        const wchar_t* external_file_name{};
        const wchar_t* legacy_relative_path{};
    };
    struct Declaration
    {
        std::uint32_t size{sizeof(Declaration)};
        const wchar_t* id{};
        const wchar_t* version{};
        const wchar_t* display_name{};
        const wchar_t* url{};
        const wchar_t* archive_name{};
        const wchar_t* sha256{};
        std::uint64_t archive_size{};
        const File* files{};
        std::uint32_t file_count{};
        const Entry* entries{};
        std::uint32_t entry_count{};
        const wchar_t* description_key{};
    };
    struct Cancellation
    {
        void* context{};
        BOOL(WINAPI* requested)(void*) noexcept{};
    };
    struct OutputSink
    {
        void* context{};
        BOOL(WINAPI* append)(void*, BOOL standard_error, const char*, std::uint32_t) noexcept{};
    };
    struct ProcessRequest
    {
        std::uint32_t size{sizeof(ProcessRequest)};
        const wchar_t* dependency_id{};
        const wchar_t* entry_id{};
        const wchar_t* const* arguments{};
        std::uint32_t argument_count{};
        std::uint32_t timeout_ms{10000};
        std::uint32_t maximum_output_bytes{8 * 1024 * 1024};
        Cancellation cancellation;
        OutputSink output;
    };
    struct ProcessResult
    {
        std::uint32_t size{sizeof(ProcessResult)};
        std::uint32_t exit_code{};
        HRESULT result{E_FAIL};
    };

    // Host-owned table remains valid until component shutdown. Declarations are
    // copied during registration. Blocking invocation is only for worker threads.
    // Library handles must be released through this table before shutdown.
    struct HostApi
    {
        std::uint32_t size{sizeof(HostApi)};
        std::uint32_t version{host_api_version};
        HRESULT(WINAPI* register_dependency)(const Declaration*, const wchar_t* consumer_id) noexcept{};
        Availability(WINAPI* query)(const wchar_t* dependency_id, const wchar_t* entry_id) noexcept{};
        HRESULT(WINAPI* execute)(const ProcessRequest*, ProcessResult*) noexcept{};
        HRESULT(WINAPI* load_library)(const wchar_t* dependency_id, const wchar_t* entry_id, std::uint64_t* lease) noexcept{};
        FARPROC(WINAPI* find_symbol)(std::uint64_t lease, const char* name) noexcept{};
        void(WINAPI* release_library)(std::uint64_t lease) noexcept{};
    };
}

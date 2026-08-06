#include "pch.h"

#include "../Common/component_localization.h"
#include "../../version.h"
#include "archive_protocol.h"
#include "glance/contracts/component_api.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    using namespace glance::contracts::components;
    using namespace glance::components::archive;

    constexpr std::array supported_extensions{
        L".zip", L".7z", L".rar", L".tar", L".gz", L".bz2", L".xz",
        L".tgz", L".tbz", L".tbz2", L".txz", L".zst", L".cab",
        L".iso", L".udf", L".wim", L".ar", L".cpio", L".lzh", L".lha",
        L".xar" };
    constexpr wchar_t display_name_key[] = L"Component.DisplayName";
    constexpr wchar_t status_available_key[] = L"Status.Available";
    constexpr wchar_t status_unavailable_key[] = L"Status.Unavailable";
    constexpr wchar_t loading_key[] = L"Preview.Loading";
    constexpr std::chrono::seconds host_timeout{ 20 };

    class UniqueHandle final
    {
    public:
        UniqueHandle() = default;
        explicit UniqueHandle(HANDLE value) noexcept : value_(value)
        {
        }
        UniqueHandle(const UniqueHandle&) = delete;
        UniqueHandle& operator=(const UniqueHandle&) = delete;
        UniqueHandle(UniqueHandle&& other) noexcept
            : value_(std::exchange(other.value_, nullptr))
        {
        }
        UniqueHandle& operator=(UniqueHandle&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                value_ = std::exchange(other.value_, nullptr);
            }
            return *this;
        }
        ~UniqueHandle()
        {
            reset();
        }
        void reset(HANDLE value = nullptr) noexcept
        {
            if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE)
            {
                CloseHandle(value_);
            }
            value_ = value;
        }
        [[nodiscard]] HANDLE get() const noexcept
        {
            return value_;
        }
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
        }

    private:
        HANDLE value_{};
    };

    enum class ColumnKind
    {
        name,
        type,
        modified,
        packed_size,
        original_size,
    };

    struct ArchiveNode
    {
        std::uint64_t id{};
        std::uint64_t parent{};
        std::uint32_t flags{};
        std::wstring name;
        std::wstring type;
        std::uint64_t modified{};
        std::uint64_t packed{};
        std::uint64_t size{};
    };

    struct ArchiveIndex
    {
        HostStatus status{ HostStatus::failed };
        std::uint32_t flags{};
        std::wstring format_name;
        std::uint64_t file_count{};
        std::uint64_t packed_size{};
        std::uint64_t original_size{};
        std::vector<ArchiveNode> nodes;
        std::unordered_map<std::uint64_t, std::vector<std::size_t>> children;
        std::vector<ColumnKind> columns;
        std::wstring folder_type;
        std::wstring file_type;
    };

    struct PreviewLease
    {
        std::filesystem::path source;
        std::mutex mutex;
        std::wstring password;
        std::shared_ptr<ArchiveIndex> index;
    };

    glance::components::ComponentResourceStore component_resources;
    std::mutex lease_mutex;
    std::unordered_map<std::uint64_t, std::shared_ptr<PreviewLease>> leases;
    std::atomic_uint64_t next_lease_token{ 1 };
    std::atomic_bool shutting_down{};

    template <std::size_t Size>
    bool localize(
        const wchar_t* key,
        const wchar_t* language_tag,
        wchar_t (&destination)[Size]) noexcept
    {
        return component_resources.copy(key, language_tag, destination, Size);
    }

    std::wstring localized_string(const wchar_t* key, const wchar_t* language_tag)
    {
        wchar_t value[file_directory_text_capacity]{};
        return localize(key, language_tag, value) ? std::wstring(value) : std::wstring{};
    }

    std::filesystem::path component_directory() noexcept
    {
        HMODULE module{};
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&component_directory),
                &module))
        {
            return {};
        }
        std::wstring path(32768, L'\0');
        const DWORD length =
            GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size())
        {
            return {};
        }
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

    std::wstring lower_extension(const std::filesystem::path& path)
    {
        auto extension = path.extension().wstring();
        std::ranges::transform(extension, extension.begin(), [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
        return extension;
    }

    bool supported_extension(std::wstring_view extension) noexcept
    {
        return std::ranges::find(supported_extensions, extension) !=
            supported_extensions.end();
    }

    std::wstring handle_argument(HANDLE handle)
    {
        return std::to_wstring(reinterpret_cast<std::uintptr_t>(handle));
    }

    std::wstring quote_argument(std::wstring_view value)
    {
        std::wstring result(L"\"");
        std::size_t backslashes{};
        for (const wchar_t character : value)
        {
            if (character == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (character == L'\"')
            {
                result.append(backslashes * 2 + 1, L'\\');
                result.push_back(character);
                backslashes = 0;
                continue;
            }
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(character);
        }
        result.append(backslashes * 2, L'\\');
        result.push_back(L'\"');
        return result;
    }

    bool write_exact(HANDLE output, const void* data, std::size_t size)
    {
        const auto* bytes = static_cast<const std::byte*>(data);
        while (size != 0)
        {
            DWORD written{};
            const DWORD requested =
                static_cast<DWORD>(std::min<std::size_t>(size, MAXDWORD));
            if (!WriteFile(output, bytes, requested, &written, nullptr) || written == 0)
            {
                return false;
            }
            bytes += written;
            size -= written;
        }
        return true;
    }

    bool append_pipe_output(HANDLE input, std::vector<std::byte>& output)
    {
        std::array<std::byte, 64 * 1024> buffer{};
        for (;;)
        {
            DWORD read{};
            if (!ReadFile(
                    input,
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()),
                    &read,
                    nullptr))
            {
                return GetLastError() == ERROR_BROKEN_PIPE;
            }
            if (read == 0)
            {
                return true;
            }
            if (output.size() > maximum_response_bytes - read)
            {
                return false;
            }
            output.insert(output.end(), buffer.begin(), buffer.begin() + read);
        }
    }

    bool parse_string(
        const std::vector<std::byte>& bytes,
        std::size_t& offset,
        std::uint32_t characters,
        std::wstring& value)
    {
        if (characters > 32767 ||
            characters > (bytes.size() - offset) / sizeof(wchar_t))
        {
            return false;
        }
        const auto* begin = reinterpret_cast<const wchar_t*>(bytes.data() + offset);
        value.assign(begin, begin + characters);
        offset += static_cast<std::size_t>(characters) * sizeof(wchar_t);
        return true;
    }

    template <typename Value>
    bool parse_value(
        const std::vector<std::byte>& bytes,
        std::size_t& offset,
        Value& value)
    {
        if (sizeof(Value) > bytes.size() - offset)
        {
            return false;
        }
        std::memcpy(&value, bytes.data() + offset, sizeof(Value));
        offset += sizeof(Value);
        return true;
    }

    std::shared_ptr<ArchiveIndex> parse_host_response(
        const std::vector<std::byte>& response)
    {
        std::size_t offset{};
        ResponseHeader header;
        if (!parse_value(response, offset, header) ||
            header.magic != response_magic ||
            header.version != protocol_version ||
            header.entry_count > maximum_entries)
        {
            return {};
        }
        auto index = std::make_shared<ArchiveIndex>();
        index->status = header.status;
        index->flags = header.flags;
        index->file_count = header.file_count;
        index->packed_size = header.packed_size;
        index->original_size = header.original_size;
        if (!parse_string(
                response,
                offset,
                header.format_name_characters,
                index->format_name))
        {
            return {};
        }
        index->nodes.reserve(header.entry_count);
        for (std::uint32_t entry_index = 0;
             entry_index < header.entry_count;
             ++entry_index)
        {
            EntryHeader header_entry;
            ArchiveNode node;
            if (!parse_value(response, offset, header_entry) ||
                header_entry.node_id == 0 ||
                header_entry.parent_id > header.entry_count ||
                !parse_string(
                    response,
                    offset,
                    header_entry.name_characters,
                    node.name) ||
                !parse_string(
                    response,
                    offset,
                    header_entry.type_characters,
                    node.type))
            {
                return {};
            }
            node.id = header_entry.node_id;
            node.parent = header_entry.parent_id;
            node.flags = header_entry.flags;
            node.modified = header_entry.modified_time;
            node.packed = header_entry.packed_size;
            node.size = header_entry.original_size;
            index->children[node.parent].push_back(index->nodes.size());
            index->nodes.push_back(std::move(node));
        }
        if (offset != response.size())
        {
            return {};
        }
        for (auto& [parent, children] : index->children)
        {
            static_cast<void>(parent);
            std::ranges::stable_sort(children, [&index](std::size_t left, std::size_t right) {
                const auto& left_node = index->nodes[left];
                const auto& right_node = index->nodes[right];
                const bool left_folder = (left_node.flags & entry_is_folder) != 0;
                const bool right_folder = (right_node.flags & entry_is_folder) != 0;
                if (left_folder != right_folder)
                {
                    return left_folder;
                }
                return _wcsicmp(left_node.name.c_str(), right_node.name.c_str()) < 0;
            });
        }
        return index;
    }

    std::shared_ptr<ArchiveIndex> run_host(
        const std::filesystem::path& source,
        std::wstring_view password)
    {
        const auto directory = component_directory();
        const auto host = directory / L"Glance.ArchiveHost.exe";
        std::error_code error;
        if (!std::filesystem::is_regular_file(host, error))
        {
            return {};
        }

        SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
        UniqueHandle child_input;
        UniqueHandle parent_input;
        UniqueHandle parent_output;
        UniqueHandle child_output;
        HANDLE input_read{};
        HANDLE input_write{};
        HANDLE output_read{};
        HANDLE output_write{};
        if (!CreatePipe(&input_read, &input_write, &security, 0) ||
            !CreatePipe(&output_read, &output_write, &security, 0))
        {
            if (input_read != nullptr)
            {
                CloseHandle(input_read);
            }
            if (input_write != nullptr)
            {
                CloseHandle(input_write);
            }
            if (output_read != nullptr)
            {
                CloseHandle(output_read);
            }
            if (output_write != nullptr)
            {
                CloseHandle(output_write);
            }
            return {};
        }
        child_input.reset(input_read);
        parent_input.reset(input_write);
        parent_output.reset(output_read);
        child_output.reset(output_write);
        if (!SetHandleInformation(parent_input.get(), HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(parent_output.get(), HANDLE_FLAG_INHERIT, 0))
        {
            return {};
        }

        SIZE_T attribute_size{};
        static_cast<void>(InitializeProcThreadAttributeList(
            nullptr,
            1,
            0,
            &attribute_size));
        std::vector<std::byte> attribute_storage(attribute_size);
        auto* attributes =
            reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size))
        {
            return {};
        }
        HANDLE inherited_handles[]{ child_input.get(), child_output.get() };
        if (!UpdateProcThreadAttribute(
                attributes,
                0,
                PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                inherited_handles,
                sizeof(inherited_handles),
                nullptr,
                nullptr))
        {
            DeleteProcThreadAttributeList(attributes);
            return {};
        }

        auto command = quote_argument(host.wstring()) + L" " +
            handle_argument(child_input.get()) + L" " +
            handle_argument(child_output.get());
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(
            host.c_str(),
            command.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
            nullptr,
            directory.c_str(),
            &startup.StartupInfo,
            &process);
        DeleteProcThreadAttributeList(attributes);
        if (!created)
        {
            return {};
        }
        UniqueHandle process_handle(process.hProcess);
        UniqueHandle thread_handle(process.hThread);
        UniqueHandle process_job(CreateJobObjectW(nullptr, nullptr));
        if (process_job)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(
                    process_job.get(),
                    JobObjectExtendedLimitInformation,
                    &limits,
                    sizeof(limits)) ||
                !AssignProcessToJobObject(process_job.get(), process_handle.get()))
            {
                process_job.reset();
            }
        }
        if (ResumeThread(thread_handle.get()) == static_cast<DWORD>(-1))
        {
            TerminateProcess(process_handle.get(), ERROR_PROCESS_ABORTED);
            return {};
        }
        thread_handle.reset();
        child_input.reset();
        child_output.reset();

        std::vector<std::byte> response;
        std::atomic_bool read_succeeded{};
        std::jthread reader([&] {
            read_succeeded = append_pipe_output(parent_output.get(), response);
        });
        const auto path = source.wstring();
        const RequestHeader request{
            .path_characters = static_cast<std::uint32_t>(path.size()),
            .password_characters = static_cast<std::uint32_t>(password.size()) };
        const bool request_written =
            write_exact(parent_input.get(), &request, sizeof(request)) &&
            write_exact(parent_input.get(), path.data(), path.size() * sizeof(wchar_t)) &&
            write_exact(
                parent_input.get(),
                password.data(),
                password.size() * sizeof(wchar_t));
        parent_input.reset();

        const auto deadline = std::chrono::steady_clock::now() + host_timeout;
        DWORD wait_result = WAIT_TIMEOUT;
        while (request_written && !shutting_down.load(std::memory_order_relaxed) &&
               std::chrono::steady_clock::now() < deadline)
        {
            wait_result = WaitForSingleObject(process_handle.get(), 100);
            if (wait_result != WAIT_TIMEOUT)
            {
                break;
            }
        }
        if (wait_result != WAIT_OBJECT_0)
        {
            TerminateProcess(
                process_handle.get(),
                shutting_down.load(std::memory_order_relaxed)
                    ? ERROR_CANCELLED
                    : ERROR_TIMEOUT);
            static_cast<void>(WaitForSingleObject(process_handle.get(), 5000));
        }
        reader.join();
        DWORD exit_code = ERROR_PROCESS_ABORTED;
        static_cast<void>(GetExitCodeProcess(process_handle.get(), &exit_code));
        process_job.reset();
        if (!request_written || wait_result != WAIT_OBJECT_0 ||
            exit_code != ERROR_SUCCESS || !read_succeeded.load())
        {
            return {};
        }
        return parse_host_response(response);
    }

    std::shared_ptr<PreviewLease> find_lease(std::uint64_t token)
    {
        std::scoped_lock lock(lease_mutex);
        const auto found = leases.find(token);
        return found == leases.end() ? nullptr : found->second;
    }

    bool copy_text(
        std::wstring_view value,
        wchar_t* destination,
        std::size_t capacity)
    {
        if (value.size() >= capacity)
        {
            return false;
        }
        std::copy(value.begin(), value.end(), destination);
        destination[value.size()] = L'\0';
        return true;
    }

    bool add_info_field(
        FileDirectoryDescriptor& descriptor,
        const wchar_t* id,
        const wchar_t* label_key,
        const wchar_t* language_tag,
        FileDirectoryValueKind kind,
        std::uint64_t unsigned_value = 0,
        double ratio_value = 0.0,
        std::wstring_view text = {})
    {
        if (descriptor.info_field_count >= maximum_file_directory_info_fields)
        {
            return false;
        }
        auto& field = descriptor.info_fields[descriptor.info_field_count];
        if (wcscpy_s(field.id, id) != 0 ||
            !localize(label_key, language_tag, field.label) ||
            !copy_text(text, field.text, std::size(field.text)))
        {
            return false;
        }
        field.kind = kind;
        field.unsigned_value = unsigned_value;
        field.ratio_value = ratio_value;
        ++descriptor.info_field_count;
        return true;
    }

    bool add_column(
        FileDirectoryDescriptor& descriptor,
        std::vector<ColumnKind>& columns,
        ColumnKind kind,
        const wchar_t* id,
        const wchar_t* title_key,
        const wchar_t* language_tag,
        FileDirectoryValueKind value_kind,
        FileDirectoryAlignment alignment,
        std::uint32_t width)
    {
        if (descriptor.column_count >= maximum_file_directory_columns)
        {
            return false;
        }
        auto& column = descriptor.columns[descriptor.column_count];
        if (wcscpy_s(column.id, id) != 0 ||
            !localize(title_key, language_tag, column.title))
        {
            return false;
        }
        column.kind = value_kind;
        column.alignment = alignment;
        column.width = width;
        columns.push_back(kind);
        ++descriptor.column_count;
        return true;
    }

    bool fill_descriptor(
        ArchiveIndex& index,
        const wchar_t* language_tag,
        FileDirectoryDescriptor& descriptor)
    {
        FileDirectoryDescriptor result;
        result.presentation = FileDirectoryPresentation::tree;
        result.truncated = (index.flags & response_truncated) != 0;
        result.depth_limited = (index.flags & response_depth_limited) != 0;
        if (!add_info_field(
                result,
                L"file-count",
                L"Info.FileCount",
                language_tag,
                FileDirectoryValueKind::unsigned_integer,
                index.file_count) ||
            !add_info_field(
                result,
                L"format",
                L"Info.Format",
                language_tag,
                FileDirectoryValueKind::text,
                0,
                0.0,
                index.format_name) ||
            !add_info_field(
                result,
                L"packed-size",
                L"Info.ArchiveSize",
                language_tag,
                FileDirectoryValueKind::bytes,
                index.packed_size))
        {
            return false;
        }
        if ((index.flags & response_has_original_size) != 0 &&
            (!add_info_field(
                 result,
                 L"original-size",
                 L"Info.OriginalSize",
                 language_tag,
                 FileDirectoryValueKind::bytes,
                 index.original_size) ||
             (index.original_size != 0 &&
              !add_info_field(
                  result,
                  L"ratio",
                  L"Info.Ratio",
                  language_tag,
                  FileDirectoryValueKind::ratio,
                  0,
                  static_cast<double>(index.packed_size) /
                      static_cast<double>(index.original_size)))))
        {
            return false;
        }
        if ((index.flags & response_has_encrypted_items) != 0 &&
            !add_info_field(
                result,
                L"encrypted",
                L"Info.Encrypted",
                language_tag,
                FileDirectoryValueKind::text,
                0,
                0.0,
                localized_string(L"Value.Yes", language_tag)))
        {
            return false;
        }

        index.columns.clear();
        if (!add_column(
                result,
                index.columns,
                ColumnKind::name,
                L"name",
                L"Column.Name",
                language_tag,
                FileDirectoryValueKind::text,
                FileDirectoryAlignment::left,
                0) ||
            !add_column(
                result,
                index.columns,
                ColumnKind::type,
                L"type",
                L"Column.Type",
                language_tag,
                FileDirectoryValueKind::text,
                FileDirectoryAlignment::left,
                100))
        {
            return false;
        }
        if ((index.flags & response_has_packed_size) != 0)
        {
            if (!add_column(
                    result,
                    index.columns,
                    ColumnKind::packed_size,
                    L"packed-size",
                    L"Column.PackedSize",
                    language_tag,
                    FileDirectoryValueKind::bytes,
                    FileDirectoryAlignment::right,
                    110))
            {
                return false;
            }
        }
        else if ((index.flags & response_has_modified_time) != 0 &&
                 !add_column(
                     result,
                     index.columns,
                     ColumnKind::modified,
                     L"modified",
                     L"Column.Modified",
                     language_tag,
                     FileDirectoryValueKind::timestamp,
                     FileDirectoryAlignment::left,
                     150))
        {
            return false;
        }
        if ((index.flags & response_has_original_size) != 0 &&
            !add_column(
                result,
                index.columns,
                ColumnKind::original_size,
                L"size",
                L"Column.Size",
                language_tag,
                FileDirectoryValueKind::bytes,
                FileDirectoryAlignment::right,
                110))
        {
            return false;
        }
        index.folder_type = localized_string(L"Value.Folder", language_tag);
        index.file_type = localized_string(L"Value.File", language_tag);
        if (index.folder_type.empty() || index.file_type.empty())
        {
            return false;
        }
        descriptor = result;
        return true;
    }

    BOOL WINAPI initialize(
        const ComponentRegistrar* registrar,
        ComponentRegistration* registration) noexcept
    {
        if (registrar == nullptr ||
            registrar->size < sizeof(ComponentRegistrar) ||
            registrar->register_extension == nullptr ||
            registrar->register_renderer == nullptr ||
            registration == nullptr ||
            registration->size < sizeof(ComponentRegistration) ||
            !component_resources.initialize())
        {
            return FALSE;
        }
        for (const auto* extension : supported_extensions)
        {
            if (!registrar->register_extension(registrar->context, extension))
            {
                return FALSE;
            }
        }
        if (!registrar->register_renderer(
                registrar->context,
                PreviewContentKind::directory,
                PreviewContentFormat::file_directory,
                &file_directory_preview_api_id,
                file_directory_preview_api_version))
        {
            return FALSE;
        }
        ComponentRegistration result;
        wcscpy_s(result.component_id, L"archive");
        wcscpy_s(result.target_app_version, GLANCE_VERSION_WSTRING);
        result.preferred_kind = PreviewContentKind::directory;
        result.preferred_format = PreviewContentFormat::file_directory;
        *registration = result;
        return TRUE;
    }

    BOOL WINAPI query_status(
        const wchar_t* language_tag,
        ComponentStatusResult* result) noexcept
    {
        if (result == nullptr || result->size < sizeof(ComponentStatusResult))
        {
            return FALSE;
        }
        const auto directory = component_directory();
        std::error_code error;
        const bool available =
            std::filesystem::is_regular_file(
                directory / L"Glance.ArchiveHost.exe", error) &&
            std::filesystem::is_regular_file(directory / L"7z.dll", error);
        ComponentStatusResult status;
        status.severity = available ? HealthSeverity::healthy : HealthSeverity::error;
        if (!localize(display_name_key, language_tag, status.display_name) ||
            !localize(
                available ? status_available_key : status_unavailable_key,
                language_tag,
                status.detail))
        {
            return FALSE;
        }
        *result = status;
        return TRUE;
    }

    BOOL WINAPI query_loading_text(
        const wchar_t* path,
        const wchar_t* language_tag,
        ComponentLoadingTextResult* result) noexcept
    {
        if (path == nullptr || result == nullptr ||
            result->size < sizeof(ComponentLoadingTextResult))
        {
            return FALSE;
        }
        ComponentLoadingTextResult text;
        if (!localize(loading_key, language_tag, text.text))
        {
            return FALSE;
        }
        *result = text;
        return TRUE;
    }

    BOOL WINAPI can_preview(const wchar_t* path) noexcept
    {
        return path != nullptr && supported_extension(lower_extension(path));
    }

    PrepareStatus WINAPI prepare_preview(
        const wchar_t* path,
        const wchar_t*,
        PreparedPreview* preview) noexcept
    {
        if (path == nullptr || preview == nullptr ||
            preview->size < sizeof(PreparedPreview))
        {
            return PrepareStatus::failed;
        }
        try
        {
            const std::filesystem::path source(path);
            std::error_code error;
            if (!source.is_absolute() ||
                !std::filesystem::is_regular_file(source, error) ||
                !supported_extension(lower_extension(source)))
            {
                return PrepareStatus::unavailable;
            }
            const auto token = next_lease_token.fetch_add(1, std::memory_order_relaxed);
            auto lease = std::make_shared<PreviewLease>();
            lease->source = source;
            {
                std::scoped_lock lock(lease_mutex);
                leases.emplace(token, std::move(lease));
            }
            PreparedPreview prepared;
            prepared.kind = PreviewContentKind::directory;
            prepared.format = PreviewContentFormat::file_directory;
            prepared.lease_token = token;
            *preview = prepared;
            return PrepareStatus::success;
        }
        catch (...)
        {
            return PrepareStatus::failed;
        }
    }

    void WINAPI release_preview(std::uint64_t token) noexcept
    {
        std::scoped_lock lock(lease_mutex);
        leases.erase(token);
    }

    FileDirectoryOpenStatus WINAPI open_directory(
        std::uint64_t token,
        const wchar_t* language_tag,
        const wchar_t* password,
        FileDirectoryDescriptor* descriptor) noexcept
    {
        if (descriptor == nullptr || descriptor->size < sizeof(FileDirectoryDescriptor))
        {
            return FileDirectoryOpenStatus::failed;
        }
        try
        {
            const auto lease = find_lease(token);
            if (lease == nullptr)
            {
                return FileDirectoryOpenStatus::cancelled;
            }
            const std::wstring requested_password = password == nullptr ? L"" : password;
            std::scoped_lock lock(lease->mutex);
            if (lease->index == nullptr || lease->password != requested_password)
            {
                lease->index = run_host(lease->source, requested_password);
                lease->password = requested_password;
            }
            if (lease->index == nullptr)
            {
                return shutting_down.load(std::memory_order_relaxed)
                    ? FileDirectoryOpenStatus::cancelled
                    : FileDirectoryOpenStatus::failed;
            }
            switch (lease->index->status)
            {
            case HostStatus::password_required:
                return FileDirectoryOpenStatus::password_required;
            case HostStatus::invalid_password:
                return FileDirectoryOpenStatus::invalid_password;
            case HostStatus::ready:
                break;
            default:
                return FileDirectoryOpenStatus::failed;
            }
            return fill_descriptor(*lease->index, language_tag, *descriptor)
                ? FileDirectoryOpenStatus::ready
                : FileDirectoryOpenStatus::failed;
        }
        catch (...)
        {
            return FileDirectoryOpenStatus::failed;
        }
    }

    BOOL WINAPI enumerate_children(
        std::uint64_t token,
        std::uint64_t parent_node_id,
        std::uint32_t offset,
        std::uint32_t limit,
        const FileDirectoryEntrySink* sink,
        std::uint32_t* returned,
        std::uint32_t* total) noexcept
    {
        if (sink == nullptr || sink->size < sizeof(FileDirectoryEntrySink) ||
            sink->append == nullptr || returned == nullptr || total == nullptr ||
            limit == 0)
        {
            return FALSE;
        }
        *returned = 0;
        *total = 0;
        try
        {
            const auto lease = find_lease(token);
            if (lease == nullptr)
            {
                return FALSE;
            }
            std::scoped_lock lock(lease->mutex);
            if (lease->index == nullptr || lease->index->status != HostStatus::ready)
            {
                return FALSE;
            }
            const auto found = lease->index->children.find(parent_node_id);
            if (found == lease->index->children.end())
            {
                return TRUE;
            }
            const auto& children = found->second;
            *total = static_cast<std::uint32_t>(children.size());
            const auto begin = std::min<std::size_t>(offset, children.size());
            const auto end = std::min<std::size_t>(
                children.size(),
                begin + static_cast<std::size_t>(limit));
            for (std::size_t child = begin; child < end; ++child)
            {
                const auto& node = lease->index->nodes[children[child]];
                std::vector<std::wstring> texts;
                texts.reserve(lease->index->columns.size());
                std::vector<FileDirectoryValue> values;
                values.reserve(lease->index->columns.size());
                for (const auto column : lease->index->columns)
                {
                    FileDirectoryValue value;
                    switch (column)
                    {
                    case ColumnKind::name:
                        value.kind = FileDirectoryValueKind::text;
                        texts.push_back(node.name);
                        value.text = texts.back().c_str();
                        break;
                    case ColumnKind::type:
                        value.kind = FileDirectoryValueKind::text;
                        texts.push_back(
                            (node.flags & entry_is_folder) != 0
                                ? lease->index->folder_type
                                : node.type.empty()
                                    ? lease->index->file_type
                                    : node.type + L" " + lease->index->file_type);
                        value.text = texts.back().c_str();
                        break;
                    case ColumnKind::modified:
                        if ((node.flags & entry_has_modified_time) != 0)
                        {
                            value.kind = FileDirectoryValueKind::timestamp;
                            value.unsigned_value = node.modified;
                        }
                        break;
                    case ColumnKind::packed_size:
                        if ((node.flags & entry_has_packed_size) != 0)
                        {
                            value.kind = FileDirectoryValueKind::bytes;
                            value.unsigned_value = node.packed;
                        }
                        break;
                    case ColumnKind::original_size:
                        if ((node.flags & entry_has_original_size) != 0)
                        {
                            value.kind = FileDirectoryValueKind::bytes;
                            value.unsigned_value = node.size;
                        }
                        break;
                    }
                    values.push_back(value);
                }
                FileDirectoryEntry entry{
                    .node_id = node.id,
                    .is_folder = (node.flags & entry_is_folder) != 0,
                    .has_children = (node.flags & entry_has_children) != 0,
                    .name = node.name.c_str(),
                    .icon_key = node.name.c_str(),
                    .value_count = static_cast<std::uint32_t>(values.size()),
                    .values = values.data() };
                if (!sink->append(sink->context, &entry))
                {
                    return FALSE;
                }
                ++*returned;
            }
            return TRUE;
        }
        catch (...)
        {
            return FALSE;
        }
    }

    const FileDirectoryPreviewApi file_directory_api{
        .open = open_directory,
        .enumerate_children = enumerate_children };

    BOOL WINAPI query_interface(
        const GUID* interface_id,
        std::uint32_t minimum_version,
        void** interface_pointer) noexcept
    {
        if (interface_pointer == nullptr)
        {
            return FALSE;
        }
        *interface_pointer = nullptr;
        if (interface_id == nullptr ||
            minimum_version > file_directory_preview_api_version ||
            !IsEqualGUID(*interface_id, file_directory_preview_api_id))
        {
            return FALSE;
        }
        *interface_pointer = const_cast<FileDirectoryPreviewApi*>(&file_directory_api);
        return TRUE;
    }

    void WINAPI shutdown() noexcept
    {
        shutting_down = true;
        {
            std::scoped_lock lock(lease_mutex);
            leases.clear();
        }
        component_resources.shutdown();
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI GlanceComponentGetApi(
    std::uint32_t host_abi,
    glance::contracts::components::ComponentApi* api) noexcept
{
    using namespace glance::contracts::components;
    if (host_abi != abi_version || api == nullptr || api->size < sizeof(ComponentApi))
    {
        return FALSE;
    }
    ComponentApi result;
    result.initialize = initialize;
    result.query_status = query_status;
    result.query_loading_text = query_loading_text;
    result.can_preview = can_preview;
    result.prepare_preview = prepare_preview;
    result.release_preview = release_preview;
    result.query_interface = query_interface;
    result.shutdown = shutdown;
    *api = result;
    return TRUE;
}

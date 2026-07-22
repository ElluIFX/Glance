#include "pch.h"
#include "archive_provider.h"
#include "localization.h"

#include <propkey.h>
#include <propsys.h>
#include <shlguid.h>
#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <optional>
#include <sstream>
#include <winrt/base.h>

namespace glance::app
{
    namespace
    {
        constexpr std::size_t maximum_archive_depth = 6;

        class unique_find_handle
        {
        public:
            explicit unique_find_handle(HANDLE value = INVALID_HANDLE_VALUE) noexcept
                : value_(value)
            {
            }

            ~unique_find_handle()
            {
                if (value_ != INVALID_HANDLE_VALUE)
                {
                    FindClose(value_);
                }
            }

            unique_find_handle(const unique_find_handle&) = delete;
            unique_find_handle& operator=(const unique_find_handle&) = delete;

            [[nodiscard]] HANDLE get() const noexcept
            {
                return value_;
            }

        private:
            HANDLE value_;
        };

        class unique_handle
        {
        public:
            explicit unique_handle(HANDLE value = nullptr) noexcept
                : value_(value)
            {
            }

            ~unique_handle()
            {
                if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(value_);
                }
            }

            unique_handle(const unique_handle&) = delete;
            unique_handle& operator=(const unique_handle&) = delete;

            [[nodiscard]] HANDLE get() const noexcept
            {
                return value_;
            }

            [[nodiscard]] bool valid() const noexcept
            {
                return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
            }

        private:
            HANDLE value_;
        };

        class unique_map_view
        {
        public:
            explicit unique_map_view(const void* value = nullptr) noexcept
                : value_(value)
            {
            }

            ~unique_map_view()
            {
                if (value_ != nullptr)
                {
                    UnmapViewOfFile(value_);
                }
            }

            unique_map_view(const unique_map_view&) = delete;
            unique_map_view& operator=(const unique_map_view&) = delete;

            [[nodiscard]] const std::byte* get() const noexcept
            {
                return static_cast<const std::byte*>(value_);
            }

        private:
            const void* value_;
        };

        bool add_size(std::uint64_t& total, std::uint64_t value) noexcept
        {
            if (std::numeric_limits<std::uint64_t>::max() - total < value)
            {
                return false;
            }
            total += value;
            return true;
        }

        void record_archive_file(
            ArchivePreview& preview,
            std::uint64_t original_size,
            bool original_size_known) noexcept
        {
            ++preview.file_count;
            if (!original_size_known || !add_size(preview.original_size, original_size))
            {
                preview.original_size_known = false;
            }
        }

        void read_archive_file_size(const std::wstring& path, ArchivePreview& preview) noexcept
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            {
                return;
            }
            preview.compressed_size =
                (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32U) |
                data.nFileSizeLow;
            preview.compressed_size_known = true;
        }

        std::uint16_t read_uint16(const std::byte* value) noexcept
        {
            return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(value[0])) |
                (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(value[1])) << 8U);
        }

        std::uint32_t read_uint32(const std::byte* value) noexcept
        {
            return static_cast<std::uint32_t>(read_uint16(value)) |
                (static_cast<std::uint32_t>(read_uint16(value + 2)) << 16U);
        }

        std::uint64_t read_uint64(const std::byte* value) noexcept
        {
            return static_cast<std::uint64_t>(read_uint32(value)) |
                (static_cast<std::uint64_t>(read_uint32(value + 4)) << 32U);
        }

        std::wstring compact_type_name(std::wstring value)
        {
            constexpr std::wstring_view suffix = L" file";
            if (value.size() >= suffix.size() &&
                _wcsicmp(value.substr(value.size() - suffix.size()).c_str(), suffix.data()) == 0)
            {
                value.resize(value.size() - suffix.size());
            }
            return value;
        }

        std::wstring archive_type_name(std::wstring_view name, bool is_folder)
        {
            if (is_folder)
            {
                return localize(L"FolderType");
            }
            auto extension = std::filesystem::path(name).extension().wstring();
            std::ranges::transform(extension, extension.begin(), [](wchar_t value) {
                return static_cast<wchar_t>(std::towupper(value));
            });
            return extension.empty() ? localize(L"FileType") : extension.substr(1);
        }

        void sort_archive_tree(std::vector<ArchiveEntry>& entries)
        {
            std::ranges::sort(entries, [](const ArchiveEntry& left, const ArchiveEntry& right) {
                if (left.is_folder != right.is_folder)
                {
                    return left.is_folder > right.is_folder;
                }
                return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
            });
            for (auto& entry : entries)
            {
                sort_archive_tree(entry.children);
            }
        }

        std::vector<std::wstring> split_archive_path(std::wstring_view value)
        {
            std::vector<std::wstring> components;
            std::wstring component;
            const auto append_component = [&components, &component]() -> bool {
                if (component.empty() || component == L".")
                {
                    component.clear();
                    return true;
                }
                if (component == L"..")
                {
                    return false;
                }
                components.push_back(std::move(component));
                component.clear();
                return true;
            };

            for (const wchar_t character : value)
            {
                if (character == L'/' || character == L'\\')
                {
                    if (!append_component())
                    {
                        return {};
                    }
                }
                else
                {
                    component.push_back(character);
                }
            }
            if (!append_component())
            {
                return {};
            }
            return components;
        }

        bool insert_archive_path(
            ArchivePreview& preview,
            std::wstring_view value,
            bool is_folder,
            std::size_t maximum_entries,
            std::uint64_t compressed_size = 0,
            bool compressed_size_known = false,
            std::uint64_t original_size = 0,
            bool original_size_known = false)
        {
            const auto components = split_archive_path(value);
            if (components.empty())
            {
                return true;
            }
            if (components.size() > maximum_archive_depth)
            {
                preview.depth_limited = true;
            }

            auto* siblings = &preview.entries;
            const std::size_t visible_depth = std::min(components.size(), maximum_archive_depth);
            for (std::size_t index = 0; index < visible_depth; ++index)
            {
                const bool is_visible_leaf = index + 1 == components.size();
                const bool node_is_folder = !is_visible_leaf || is_folder;
                auto existing = std::ranges::find(*siblings, components[index], &ArchiveEntry::name);
                if (existing == siblings->end())
                {
                    if (preview.entry_count >= maximum_entries)
                    {
                        preview.truncated = true;
                        preview.entry_limit_reached = true;
                        return false;
                    }
                    ArchiveEntry entry;
                    entry.name = components[index];
                    entry.path = entry.name;
                    entry.is_folder = node_is_folder;
                    entry.type_name = archive_type_name(entry.name, entry.is_folder);
                    siblings->push_back(std::move(entry));
                    existing = std::prev(siblings->end());
                    ++preview.entry_count;
                }
                else if (node_is_folder && !existing->is_folder)
                {
                    existing->is_folder = true;
                    existing->type_name = localize(L"FolderType");
                }
                if (is_visible_leaf && !node_is_folder)
                {
                    existing->compressed_size = compressed_size;
                    existing->compressed_size_known = compressed_size_known;
                    existing->original_size = original_size;
                    existing->original_size_known = original_size_known;
                }
                siblings = &existing->children;
            }
            return true;
        }

        std::wstring decode_bytes(const char* value, std::size_t size, UINT code_page, DWORD flags = 0)
        {
            if (size == 0 || size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            {
                return {};
            }
            const int length = MultiByteToWideChar(
                code_page,
                flags,
                value,
                static_cast<int>(size),
                nullptr,
                0);
            if (length <= 0)
            {
                return {};
            }
            std::wstring result(static_cast<std::size_t>(length), L'\0');
            MultiByteToWideChar(
                code_page,
                flags,
                value,
                static_cast<int>(size),
                result.data(),
                length);
            return result;
        }

        std::wstring decode_zip_name(
            const std::byte* name,
            std::size_t name_length,
            std::uint16_t flags,
            const std::byte* extra,
            std::size_t extra_length)
        {
            std::size_t offset{};
            while (offset + 4 <= extra_length)
            {
                const auto identifier = read_uint16(extra + offset);
                const auto length = read_uint16(extra + offset + 2);
                offset += 4;
                if (length > extra_length - offset)
                {
                    break;
                }
                if (identifier == 0x7075 && length > 5 &&
                    std::to_integer<std::uint8_t>(extra[offset]) == 1)
                {
                    auto unicode_name = decode_bytes(
                        reinterpret_cast<const char*>(extra + offset + 5),
                        length - 5,
                        CP_UTF8,
                        MB_ERR_INVALID_CHARS);
                    if (!unicode_name.empty())
                    {
                        return unicode_name;
                    }
                }
                offset += length;
            }

            const auto code_page = (flags & (1U << 11U)) != 0 ? CP_UTF8 : 437U;
            auto decoded = decode_bytes(
                reinterpret_cast<const char*>(name),
                name_length,
                code_page,
                code_page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0);
            if (decoded.empty() && name_length != 0)
            {
                decoded = decode_bytes(
                    reinterpret_cast<const char*>(name),
                    name_length,
                    CP_ACP);
            }
            return decoded;
        }

        bool read_zip64_sizes(
            const std::byte* extra,
            std::size_t extra_length,
            bool needs_original_size,
            bool needs_compressed_size,
            std::uint64_t& original_size,
            std::uint64_t& compressed_size) noexcept
        {
            std::size_t offset{};
            while (offset + 4 <= extra_length)
            {
                const auto identifier = read_uint16(extra + offset);
                const auto length = read_uint16(extra + offset + 2);
                offset += 4;
                if (length > extra_length - offset)
                {
                    return false;
                }
                if (identifier != 0x0001)
                {
                    offset += length;
                    continue;
                }

                std::size_t value_offset{};
                if (needs_original_size)
                {
                    if (value_offset + 8 > length)
                    {
                        return false;
                    }
                    original_size = read_uint64(extra + offset + value_offset);
                    value_offset += 8;
                }
                if (needs_compressed_size)
                {
                    if (value_offset + 8 > length)
                    {
                        return false;
                    }
                    compressed_size = read_uint64(extra + offset + value_offset);
                }
                return true;
            }
            return !needs_original_size && !needs_compressed_size;
        }

        ArchivePreview load_zip_archive_preview(
            const std::wstring& path,
            std::size_t maximum_entries)
        {
            ArchivePreview result;
            result.original_size_known = true;
            result.entry_compressed_size_available = true;

            unique_handle file(CreateFileW(
                path.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                nullptr));
            LARGE_INTEGER file_size{};
            if (!file.valid() || !GetFileSizeEx(file.get(), &file_size) ||
                file_size.QuadPart < 22 ||
                static_cast<std::uint64_t>(file_size.QuadPart) >
                    static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
            {
                result.error = localize(L"ArchiveReadError");
                return result;
            }

            result.compressed_size = static_cast<std::uint64_t>(file_size.QuadPart);
            result.compressed_size_known = true;
            unique_handle mapping(CreateFileMappingW(
                file.get(),
                nullptr,
                PAGE_READONLY,
                0,
                0,
                nullptr));
            if (!mapping.valid())
            {
                result.error = localize(L"ArchiveReadError");
                return result;
            }
            unique_map_view view(MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0, 0));
            if (view.get() == nullptr)
            {
                result.error = localize(L"ArchiveReadError");
                return result;
            }

            constexpr std::uint32_t end_signature = 0x06054B50;
            constexpr std::uint32_t zip64_locator_signature = 0x07064B50;
            constexpr std::uint32_t zip64_end_signature = 0x06064B50;
            constexpr std::uint32_t central_entry_signature = 0x02014B50;
            const auto* data = view.get();
            const auto size = static_cast<std::size_t>(file_size.QuadPart);
            const std::size_t search_start = size > 22 + 65535 ? size - 22 - 65535 : 0;
            std::optional<std::size_t> end_offset;
            for (std::size_t offset = size - 22;; --offset)
            {
                if (read_uint32(data + offset) == end_signature)
                {
                    const auto comment_length = read_uint16(data + offset + 20);
                    if (offset + 22 + comment_length <= size)
                    {
                        end_offset = offset;
                        break;
                    }
                }
                if (offset == search_start)
                {
                    break;
                }
            }
            if (!end_offset.has_value())
            {
                result.error = localize(L"ArchiveReadError");
                return result;
            }

            const auto* end = data + *end_offset;
            std::uint64_t entry_count = read_uint16(end + 10);
            std::uint64_t central_size = read_uint32(end + 12);
            std::uint64_t central_offset = read_uint32(end + 16);
            if (entry_count == 0xFFFFU || central_size == 0xFFFFFFFFULL ||
                central_offset == 0xFFFFFFFFULL)
            {
                if (*end_offset < 20)
                {
                    result.error = localize(L"ArchiveReadError");
                    return result;
                }
                const auto* locator = data + *end_offset - 20;
                if (read_uint32(locator) != zip64_locator_signature)
                {
                    result.error = localize(L"ArchiveReadError");
                    return result;
                }
                const auto zip64_offset = read_uint64(locator + 8);
                if (size < 56 || zip64_offset > static_cast<std::uint64_t>(size - 56))
                {
                    result.error = localize(L"ArchiveReadError");
                    return result;
                }
                const auto* zip64_end = data + static_cast<std::size_t>(zip64_offset);
                if (read_uint32(zip64_end) != zip64_end_signature)
                {
                    result.error = localize(L"ArchiveReadError");
                    return result;
                }
                entry_count = read_uint64(zip64_end + 32);
                central_size = read_uint64(zip64_end + 40);
                central_offset = read_uint64(zip64_end + 48);
            }

            if (central_offset > static_cast<std::uint64_t>(size) ||
                central_size > static_cast<std::uint64_t>(size) - central_offset)
            {
                result.error = localize(L"ArchiveReadError");
                return result;
            }

            std::size_t cursor = static_cast<std::size_t>(central_offset);
            const std::size_t central_end = cursor + static_cast<std::size_t>(central_size);
            for (std::uint64_t index = 0; index < entry_count; ++index)
            {
                if (cursor + 46 > central_end || read_uint32(data + cursor) != central_entry_signature)
                {
                    result.error = localize(L"ArchiveReadError");
                    return result;
                }
                const auto* header = data + cursor;
                const auto flags = read_uint16(header + 8);
                std::uint64_t compressed_size = read_uint32(header + 20);
                std::uint64_t original_size = read_uint32(header + 24);
                const auto name_length = read_uint16(header + 28);
                const auto extra_length = read_uint16(header + 30);
                const auto comment_length = read_uint16(header + 32);
                const std::size_t record_size =
                    46ULL + name_length + extra_length + comment_length;
                if (record_size > central_end - cursor)
                {
                    result.error = localize(L"ArchiveReadError");
                    return result;
                }
                const auto* name_data = header + 46;
                const auto* extra_data = name_data + name_length;
                const bool needs_original_size = original_size == 0xFFFFFFFFULL;
                const bool needs_compressed_size = compressed_size == 0xFFFFFFFFULL;
                if (!read_zip64_sizes(
                        extra_data,
                        extra_length,
                        needs_original_size,
                        needs_compressed_size,
                        original_size,
                        compressed_size))
                {
                    result.error = localize(L"ArchiveReadError");
                    return result;
                }

                auto name = decode_zip_name(
                    name_data,
                    name_length,
                    flags,
                    extra_data,
                    extra_length);
                const bool is_folder =
                    !name.empty() && (name.back() == L'/' || name.back() == L'\\');
                while (is_folder && !name.empty() &&
                       (name.back() == L'/' || name.back() == L'\\'))
                {
                    name.pop_back();
                }
                if (!is_folder)
                {
                    record_archive_file(result, original_size, true);
                    result.entry_compressed_size_available = true;
                }
                static_cast<void>(insert_archive_path(
                    result,
                    name,
                    is_folder,
                    maximum_entries,
                    compressed_size,
                    !is_folder,
                    original_size,
                    !is_folder));
                cursor += record_size;
            }

            sort_archive_tree(result.entries);
            return result;
        }

        std::wstring quote_argument(std::wstring_view value)
        {
            std::wstring result{ L'"' };
            std::size_t backslashes{};
            for (const wchar_t character : value)
            {
                if (character == L'\\')
                {
                    ++backslashes;
                    continue;
                }
                if (character == L'"')
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
            result.push_back(L'"');
            return result;
        }

        std::wstring decode_process_output(std::string_view value)
        {
            const auto decode = [value](UINT code_page, DWORD flags) -> std::wstring {
                const int length = MultiByteToWideChar(
                    code_page,
                    flags,
                    value.data(),
                    static_cast<int>(value.size()),
                    nullptr,
                    0);
                if (length <= 0)
                {
                    return {};
                }
                std::wstring result(static_cast<std::size_t>(length), L'\0');
                MultiByteToWideChar(
                    code_page,
                    flags,
                    value.data(),
                    static_cast<int>(value.size()),
                    result.data(),
                    length);
                return result;
            };
            auto result = decode(CP_UTF8, MB_ERR_INVALID_CHARS);
            return result.empty() && !value.empty() ? decode(CP_ACP, 0) : result;
        }

        struct ProcessOutput
        {
            std::string text;
            DWORD exit_code{ ERROR_GEN_FAILURE };
            bool truncated{};
        };

        std::wstring find_tar_executable()
        {
            std::wstring system_directory(32768, L'\0');
            const UINT length = GetSystemDirectoryW(
                system_directory.data(),
                static_cast<UINT>(system_directory.size()));
            if (length != 0 && length < system_directory.size())
            {
                system_directory.resize(length);
                auto candidate = std::filesystem::path(system_directory) / L"tar.exe";
                if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
                {
                    return candidate.wstring();
                }
            }

            wchar_t executable[32768]{};
            return SearchPathW(
                       nullptr,
                       L"tar.exe",
                       nullptr,
                       static_cast<DWORD>(std::size(executable)),
                       executable,
                       nullptr) != 0
                ? std::wstring(executable)
                : std::wstring{};
        }

        ProcessOutput run_tar_listing(std::wstring_view path)
        {
            ProcessOutput result;
            const auto executable = find_tar_executable();
            if (executable.empty())
            {
                return result;
            }

            SECURITY_ATTRIBUTES security{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
            HANDLE read_pipe{};
            HANDLE write_pipe{};
            if (!CreatePipe(&read_pipe, &write_pipe, &security, 0))
            {
                return result;
            }
            SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
            HANDLE null_output = CreateFileW(
                L"NUL",
                GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                &security,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);

            std::wstring command = quote_argument(executable) + L" -tvf " + quote_argument(path);
            STARTUPINFOW startup{ sizeof(STARTUPINFOW) };
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            startup.hStdOutput = write_pipe;
            startup.hStdError = null_output;
            PROCESS_INFORMATION process{};
            const BOOL created = CreateProcessW(
                executable.c_str(),
                command.data(),
                nullptr,
                nullptr,
                TRUE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &startup,
                &process);
            CloseHandle(write_pipe);
            if (null_output != INVALID_HANDLE_VALUE)
            {
                CloseHandle(null_output);
            }
            if (!created)
            {
                CloseHandle(read_pipe);
                return result;
            }

            constexpr std::size_t maximum_output = 16U * 1024U * 1024U;
            const ULONGLONG deadline = GetTickCount64() + 10000;
            bool finished{};
            while (!finished)
            {
                DWORD available{};
                if (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) && available > 0)
                {
                    std::array<char, 16384> buffer{};
                    DWORD read{};
                    const DWORD request = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
                    if (ReadFile(read_pipe, buffer.data(), request, &read, nullptr) && read > 0)
                    {
                        const std::size_t remaining = maximum_output - std::min(result.text.size(), maximum_output);
                        result.text.append(buffer.data(), std::min<std::size_t>(read, remaining));
                        if (result.text.size() == maximum_output)
                        {
                            result.truncated = true;
                            TerminateProcess(process.hProcess, ERROR_BUFFER_OVERFLOW);
                        }
                    }
                }
                finished = WaitForSingleObject(process.hProcess, 10) == WAIT_OBJECT_0;
                if (!finished && GetTickCount64() >= deadline)
                {
                    TerminateProcess(process.hProcess, ERROR_TIMEOUT);
                    WaitForSingleObject(process.hProcess, 1000);
                    finished = true;
                }
            }

            std::array<char, 16384> buffer{};
            DWORD read{};
            while (result.text.size() < maximum_output &&
                   ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read > 0)
            {
                result.text.append(buffer.data(), std::min<std::size_t>(read, maximum_output - result.text.size()));
            }
            static_cast<void>(GetExitCodeProcess(process.hProcess, &result.exit_code));
            CloseHandle(read_pipe);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            return result;
        }

        struct TarListingEntry
        {
            std::wstring name;
            std::uint64_t original_size{};
            bool is_folder{};
        };

        std::optional<TarListingEntry> parse_tar_listing_line(const std::wstring& line)
        {
            std::wistringstream fields(line);
            std::wstring permissions;
            std::wstring link_count;
            std::wstring owner;
            std::wstring group;
            std::wstring size_text;
            std::wstring month;
            std::wstring day;
            std::wstring time_or_year;
            if (!(fields >> permissions >> link_count >> owner >> group >> size_text >>
                  month >> day >> time_or_year) || permissions.empty())
            {
                return std::nullopt;
            }

            errno = 0;
            wchar_t* size_end{};
            const auto original_size = std::wcstoull(size_text.c_str(), &size_end, 10);
            if (errno == ERANGE || size_end == size_text.c_str() || *size_end != L'\0')
            {
                return std::nullopt;
            }

            std::wstring name;
            std::getline(fields, name);
            const auto first_character = name.find_first_not_of(L' ');
            if (first_character == std::wstring::npos)
            {
                return std::nullopt;
            }
            name.erase(0, first_character);
            if (permissions.front() == L'l')
            {
                if (const auto target = name.find(L" -> "); target != std::wstring::npos)
                {
                    name.resize(target);
                }
            }

            const bool is_folder =
                permissions.front() == L'd' || name.ends_with(L'/') || name.ends_with(L'\\');
            while (is_folder && !name.empty() &&
                   (name.back() == L'/' || name.back() == L'\\'))
            {
                name.pop_back();
            }
            if (name.empty())
            {
                return std::nullopt;
            }
            return TarListingEntry{
                .name = std::move(name),
                .original_size = original_size,
                .is_folder = is_folder,
            };
        }

        ArchivePreview load_tar_archive_preview(const std::wstring& path, std::size_t maximum_entries)
        {
            ArchivePreview result;
            result.original_size_known = true;
            read_archive_file_size(path, result);
            const auto output = run_tar_listing(path);
            if (output.exit_code != ERROR_SUCCESS)
            {
                result.error = localize(L"ArchiveReadError");
                return result;
            }

            std::wistringstream lines(decode_process_output(output.text));
            std::wstring line;
            while (std::getline(lines, line))
            {
                if (!line.empty() && line.back() == L'\r')
                {
                    line.pop_back();
                }
                if (line.empty())
                {
                    continue;
                }
                auto entry = parse_tar_listing_line(line);
                if (!entry.has_value())
                {
                    result.original_size_known = false;
                    continue;
                }
                if (!entry->is_folder)
                {
                    record_archive_file(result, entry->original_size, true);
                }
                static_cast<void>(insert_archive_path(
                    result,
                    entry->name,
                    entry->is_folder,
                    maximum_entries,
                    0,
                    false,
                    entry->original_size,
                    !entry->is_folder));
            }
            result.truncated = result.truncated || output.truncated;
            if (output.truncated)
            {
                result.original_size_known = false;
            }
            sort_archive_tree(result.entries);
            return result;
        }

        bool shell_folder_has_children(IShellItem* folder)
        {
            winrt::com_ptr<IEnumShellItems> enumerator;
            if (FAILED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(enumerator.put()))))
            {
                return false;
            }
            winrt::com_ptr<IShellItem> child;
            ULONG fetched{};
            return enumerator->Next(1, child.put(), &fetched) == S_OK && fetched != 0;
        }

        const std::optional<PROPERTYKEY>& compressed_size_property_key() noexcept
        {
            static const auto key = []() -> std::optional<PROPERTYKEY> {
                PROPERTYKEY value{};
                return SUCCEEDED(PSGetPropertyKeyFromName(L"System.CompressedSize", &value))
                    ? std::optional<PROPERTYKEY>(value)
                    : std::nullopt;
            }();
            return key;
        }

        bool read_shell_archive_entry(IShellItem* item, ArchiveEntry& entry)
        {
            PWSTR raw_name{};
            if (FAILED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &raw_name)))
            {
                return false;
            }
            entry.name = raw_name;
            CoTaskMemFree(raw_name);

            SFGAOF attributes{};
            if (SUCCEEDED(item->GetAttributes(SFGAO_FOLDER, &attributes)))
            {
                entry.is_folder = (attributes & SFGAO_FOLDER) != 0;
            }
            if (entry.is_folder)
            {
                entry.type_name = localize(L"FolderType");
                return true;
            }

            winrt::com_ptr<IShellItem2> item2;
            if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(item2.put()))))
            {
                entry.original_size_known =
                    SUCCEEDED(item2->GetUInt64(PKEY_Size, &entry.original_size));
                if (const auto& key = compressed_size_property_key(); key.has_value())
                {
                    entry.compressed_size_known =
                        SUCCEEDED(item2->GetUInt64(*key, &entry.compressed_size));
                }
                FILETIME modified{};
                if (SUCCEEDED(item2->GetFileTime(PKEY_DateModified, &modified)))
                {
                    entry.modified_time =
                        (static_cast<std::uint64_t>(modified.dwHighDateTime) << 32U) |
                        modified.dwLowDateTime;
                }
                PWSTR raw_type{};
                if (SUCCEEDED(item2->GetString(PKEY_ItemTypeText, &raw_type)))
                {
                    entry.type_name = compact_type_name(raw_type);
                    CoTaskMemFree(raw_type);
                }
            }
            if (entry.type_name.empty())
            {
                entry.type_name = archive_type_name(entry.name, false);
            }
            return true;
        }

        HRESULT enumerate_shell_archive_folder(
            IShellItem* folder,
            std::vector<ArchiveEntry>& destination,
            std::size_t depth,
            ArchivePreview& preview,
            std::size_t maximum_entries)
        {
            winrt::com_ptr<IEnumShellItems> enumerator;
            const HRESULT bind_result = folder->BindToHandler(
                nullptr,
                BHID_EnumItems,
                IID_PPV_ARGS(enumerator.put()));
            if (FAILED(bind_result))
            {
                return bind_result;
            }

            while (!preview.truncated)
            {
                winrt::com_ptr<IShellItem> item;
                ULONG fetched{};
                const HRESULT next_result = enumerator->Next(1, item.put(), &fetched);
                if (next_result != S_OK || fetched == 0)
                {
                    break;
                }
                if (preview.entry_count >= maximum_entries)
                {
                    preview.truncated = true;
                    preview.entry_limit_reached = true;
                    preview.original_size_known = false;
                    break;
                }

                ArchiveEntry entry;
                if (!read_shell_archive_entry(item.get(), entry))
                {
                    continue;
                }
                destination.push_back(std::move(entry));
                auto& stored = destination.back();
                ++preview.entry_count;

                if (!stored.is_folder)
                {
                    record_archive_file(
                        preview,
                        stored.original_size,
                        stored.original_size_known);
                    preview.entry_compressed_size_available =
                        preview.entry_compressed_size_available || stored.compressed_size_known;
                    continue;
                }
                if (depth < maximum_archive_depth)
                {
                    static_cast<void>(enumerate_shell_archive_folder(
                        item.get(),
                        stored.children,
                        depth + 1,
                        preview,
                        maximum_entries));
                }
                else if (shell_folder_has_children(item.get()))
                {
                    preview.depth_limited = true;
                    preview.original_size_known = false;
                }
            }
            sort_archive_tree(destination);
            return S_OK;
        }
    }

    ArchivePreview load_archive_preview(const std::wstring& path, std::size_t maximum_entries)
    {
        auto extension = std::filesystem::path(path).extension().wstring();
        std::ranges::transform(extension, extension.begin(), [](wchar_t value) {
            return static_cast<wchar_t>(std::towlower(value));
        });
        if (extension == L".zip")
        {
            auto zip_preview = load_zip_archive_preview(path, maximum_entries);
            if (zip_preview.error.empty())
            {
                return zip_preview;
            }
            auto shell_preview = load_shell_archive_preview(path, maximum_entries);
            return shell_preview.error.empty() ? std::move(shell_preview) : std::move(zip_preview);
        }

        auto shell_preview = load_shell_archive_preview(path, maximum_entries);
        if (shell_preview.error.empty() &&
            shell_preview.file_count != 0 && shell_preview.original_size_known)
        {
            return shell_preview;
        }
        auto tar_preview = load_tar_archive_preview(path, maximum_entries);
        return tar_preview.error.empty() ? std::move(tar_preview) : std::move(shell_preview);
    }

    ArchivePreview load_shell_archive_preview(
        const std::wstring& path,
        std::size_t maximum_entries)
    {
        ArchivePreview result;
        result.original_size_known = true;
        read_archive_file_size(path, result);
        const HRESULT apartment_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninitialize = SUCCEEDED(apartment_result);

        winrt::com_ptr<IShellItem> archive;
        HRESULT status = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(archive.put()));
        if (SUCCEEDED(status))
        {
            status = enumerate_shell_archive_folder(
                archive.get(),
                result.entries,
                1,
                result,
                maximum_entries);
        }

        if (FAILED(status))
        {
            result.error = localize(L"ArchiveReadError");
        }
        if (uninitialize)
        {
            CoUninitialize();
        }
        return result;
    }

    ArchivePreview load_directory_preview(
        const std::wstring& path,
        std::size_t maximum_entries)
    {
        ArchivePreview result;
        result.original_size_known = true;
        auto search_path = std::filesystem::path(path) / L"*";
        WIN32_FIND_DATAW data{};
        HANDLE raw_handle = FindFirstFileExW(
            search_path.c_str(),
            FindExInfoBasic,
            &data,
            FindExSearchNameMatch,
            nullptr,
            FIND_FIRST_EX_LARGE_FETCH);
        if (raw_handle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_PARAMETER)
        {
            raw_handle = FindFirstFileExW(
                search_path.c_str(),
                FindExInfoBasic,
                &data,
                FindExSearchNameMatch,
                nullptr,
                0);
        }
        if (raw_handle == INVALID_HANDLE_VALUE)
        {
            if (GetLastError() != ERROR_FILE_NOT_FOUND)
            {
                result.error = localize(L"FolderReadError");
            }
            return result;
        }
        unique_find_handle handle(raw_handle);

        bool has_entry = true;
        while (has_entry)
        {
            const std::wstring_view name(data.cFileName);
            if (name != L"." && name != L"..")
            {
                if (result.entry_count >= maximum_entries)
                {
                    result.truncated = true;
                    result.entry_limit_reached = true;
                    break;
                }

                ArchiveEntry entry;
                entry.name = name;
                entry.path = (std::filesystem::path(path) / entry.name).wstring();
                entry.is_folder = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                if (!entry.is_folder)
                {
                    entry.original_size =
                        (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32U) |
                        data.nFileSizeLow;
                    entry.original_size_known = true;
                    ++result.file_count;
                    if (!add_size(result.original_size, entry.original_size))
                    {
                        result.original_size_known = false;
                    }
                    entry.type_name = archive_type_name(entry.name, false);
                }
                else
                {
                    entry.type_name = localize(L"FolderType");
                }
                entry.modified_time =
                    (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32U) |
                    data.ftLastWriteTime.dwLowDateTime;
                result.entries.push_back(std::move(entry));
                ++result.entry_count;
            }
            has_entry = FindNextFileW(handle.get(), &data) != FALSE;
        }
        std::ranges::sort(result.entries, [](const ArchiveEntry& left, const ArchiveEntry& right) {
            if (left.is_folder != right.is_folder)
            {
                return left.is_folder > right.is_folder;
            }
            return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
        });
        return result;
    }
}

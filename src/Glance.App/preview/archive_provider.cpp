#include "pch.h"
#include "archive_provider.h"
#include "localization.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <ranges>

namespace glance::app
{
    namespace
    {
        class unique_find_handle final
        {
        public:
            explicit unique_find_handle(HANDLE value) noexcept : value_(value)
            {
            }

            unique_find_handle(const unique_find_handle&) = delete;
            unique_find_handle& operator=(const unique_find_handle&) = delete;

            ~unique_find_handle()
            {
                if (value_ != INVALID_HANDLE_VALUE)
                {
                    FindClose(value_);
                }
            }

            [[nodiscard]] HANDLE get() const noexcept
            {
                return value_;
            }

        private:
            HANDLE value_{};
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

        std::wstring directory_type_name(std::wstring_view name, bool is_folder)
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
    }

    ArchivePreview load_directory_preview(
        const std::wstring& path,
        std::size_t maximum_entries)
    {
        ArchivePreview result;
        result.original_size_known = true;
        const auto search_path = std::filesystem::path(path) / L"*";
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
                entry.attributes = data.dwFileAttributes;
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
                }
                entry.type_name = directory_type_name(entry.name, entry.is_folder);
                entry.creation_time =
                    (static_cast<std::uint64_t>(data.ftCreationTime.dwHighDateTime) << 32U) |
                    data.ftCreationTime.dwLowDateTime;
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

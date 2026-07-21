#include "pch.h"
#include "archive_provider.h"
#include "localization.h"

#include <propkey.h>
#include <shlguid.h>
#include <shobjidl.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <winrt/base.h>

namespace glance::app
{
    namespace
    {
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
    }

    ArchivePreview load_shell_archive_preview(
        const std::wstring& path,
        std::size_t maximum_entries)
    {
        ArchivePreview result;
        const HRESULT apartment_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninitialize = SUCCEEDED(apartment_result);

        winrt::com_ptr<IShellItem> archive;
        HRESULT status = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(archive.put()));
        if (SUCCEEDED(status))
        {
            winrt::com_ptr<IEnumShellItems> enumerator;
            status = archive->BindToHandler(
                nullptr,
                BHID_EnumItems,
                IID_PPV_ARGS(enumerator.put()));
            if (SUCCEEDED(status))
            {
                while (result.entries.size() < maximum_entries)
                {
                    winrt::com_ptr<IShellItem> item;
                    ULONG fetched{};
                    status = enumerator->Next(1, item.put(), &fetched);
                    if (status != S_OK || fetched == 0)
                    {
                        break;
                    }

                    PWSTR raw_name{};
                    if (FAILED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &raw_name)))
                    {
                        continue;
                    }
                    ArchiveEntry entry;
                    entry.name = raw_name;
                    CoTaskMemFree(raw_name);

                    SFGAOF attributes{};
                    if (SUCCEEDED(item->GetAttributes(SFGAO_FOLDER, &attributes)))
                    {
                        entry.is_folder = (attributes & SFGAO_FOLDER) != 0;
                    }
                    if (!entry.is_folder)
                    {
                        const auto item2 = item.try_as<IShellItem2>();
                        if (item2)
                        {
                            static_cast<void>(item2->GetUInt64(PKEY_Size, &entry.size));
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
                    }
                    else
                    {
                        entry.type_name = localize(L"FolderType");
                    }
                    result.entries.push_back(std::move(entry));
                }

                if (result.entries.size() == maximum_entries)
                {
                    winrt::com_ptr<IShellItem> extra;
                    ULONG fetched{};
                    result.truncated = enumerator->Next(1, extra.put(), &fetched) == S_OK && fetched != 0;
                }
            }
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
        std::error_code error;
        std::filesystem::directory_iterator iterator(
            std::filesystem::path(path),
            std::filesystem::directory_options::skip_permission_denied,
            error);
        const std::filesystem::directory_iterator end;
        if (error)
        {
            result.error = localize(L"FolderReadError");
            return result;
        }

        for (; iterator != end && result.entries.size() < maximum_entries; iterator.increment(error))
        {
            if (error)
            {
                error.clear();
                continue;
            }
            ArchiveEntry entry;
            entry.name = iterator->path().filename().wstring();
            entry.is_folder = iterator->is_directory(error);
            if (error)
            {
                error.clear();
                entry.is_folder = false;
            }
            if (!entry.is_folder)
            {
                entry.size = iterator->file_size(error);
                if (error)
                {
                    error.clear();
                    entry.size = 0;
                }
                auto extension = iterator->path().extension().wstring();
                std::ranges::transform(extension, extension.begin(), [](wchar_t value) {
                    return static_cast<wchar_t>(std::towupper(value));
                });
                entry.type_name = extension.empty() ? localize(L"FileType") : extension.substr(1);
            }
            else
            {
                entry.type_name = localize(L"FolderType");
            }
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (GetFileAttributesExW(iterator->path().c_str(), GetFileExInfoStandard, &attributes))
            {
                entry.modified_time =
                    (static_cast<std::uint64_t>(attributes.ftLastWriteTime.dwHighDateTime) << 32U) |
                    attributes.ftLastWriteTime.dwLowDateTime;
            }
            result.entries.push_back(std::move(entry));
        }
        result.truncated = iterator != end;
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

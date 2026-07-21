#include "pch.h"
#include "archive_provider.h"

#include <propkey.h>
#include <shlguid.h>
#include <shobjidl.h>

#include <winrt/base.h>

namespace glance::app
{
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
                        }
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
            result.error = L"The archive directory could not be read by the Windows ZIP handler.";
        }
        if (uninitialize)
        {
            CoUninitialize();
        }
        return result;
    }
}

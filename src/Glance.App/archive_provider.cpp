#include "pch.h"
#include "archive_provider.h"
#include "localization.h"

#include <propkey.h>
#include <shlguid.h>
#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <sstream>
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

        ProcessOutput run_tar_listing(std::wstring_view path)
        {
            ProcessOutput result;
            wchar_t executable[32768]{};
            if (SearchPathW(nullptr, L"tar.exe", nullptr, static_cast<DWORD>(std::size(executable)), executable, nullptr) == 0)
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

            std::wstring command = quote_argument(executable) + L" -tf " + quote_argument(path);
            STARTUPINFOW startup{ sizeof(STARTUPINFOW) };
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            startup.hStdOutput = write_pipe;
            startup.hStdError = null_output;
            PROCESS_INFORMATION process{};
            const BOOL created = CreateProcessW(
                executable,
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

            constexpr std::size_t maximum_output = 8U * 1024U * 1024U;
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

        ArchivePreview load_tar_archive_preview(const std::wstring& path, std::size_t maximum_entries)
        {
            ArchivePreview result;
            const auto output = run_tar_listing(path);
            if (output.exit_code != ERROR_SUCCESS)
            {
                result.error = localize(L"ArchiveReadError");
                return result;
            }

            std::wistringstream lines(decode_process_output(output.text));
            std::wstring name;
            while (std::getline(lines, name))
            {
                if (!name.empty() && name.back() == L'\r')
                {
                    name.pop_back();
                }
                if (name.empty())
                {
                    continue;
                }
                if (result.entries.size() >= maximum_entries)
                {
                    result.truncated = true;
                    break;
                }
                ArchiveEntry entry;
                entry.is_folder = name.ends_with(L'/') || name.ends_with(L'\\');
                while (entry.is_folder && !name.empty() && (name.back() == L'/' || name.back() == L'\\'))
                {
                    name.pop_back();
                }
                entry.name = std::move(name);
                if (entry.is_folder)
                {
                    entry.type_name = localize(L"FolderType");
                }
                else
                {
                    auto extension = std::filesystem::path(entry.name).extension().wstring();
                    std::ranges::transform(extension, extension.begin(), [](wchar_t value) {
                        return static_cast<wchar_t>(std::towupper(value));
                    });
                    entry.type_name = extension.empty() ? localize(L"FileType") : extension.substr(1);
                }
                result.entries.push_back(std::move(entry));
            }
            result.truncated = result.truncated || output.truncated;
            return result;
        }
    }

    ArchivePreview load_archive_preview(const std::wstring& path, std::size_t maximum_entries)
    {
        auto extension = std::filesystem::path(path).extension().wstring();
        std::ranges::transform(extension, extension.begin(), [](wchar_t value) {
            return static_cast<wchar_t>(std::towlower(value));
        });
        return extension == L".zip"
            ? load_shell_archive_preview(path, maximum_entries)
            : load_tar_archive_preview(path, maximum_entries);
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
                if (std::numeric_limits<std::uint64_t>::max() - result.total_size >= entry.size)
                {
                    result.total_size += entry.size;
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
        result.show_total_size = true;
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

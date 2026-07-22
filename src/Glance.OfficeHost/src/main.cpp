#include "../include/office_automation.h"

#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <cstdint>
#include <string>
#include <string_view>

namespace
{
    HANDLE parse_handle(const wchar_t* value)
    {
        wchar_t* end{};
        const auto numeric = _wcstoui64(value, &end, 10);
        return end != value && *end == L'\0'
            ? reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(numeric))
            : nullptr;
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argument_count{};
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (arguments == nullptr)
    {
        return 2;
    }

    if (argument_count == 6 && std::wstring_view(arguments[1]) == L"--word-session")
    {
        const std::wstring input_path = arguments[2];
        const std::wstring cache_directory = arguments[3];
        const HANDLE request_pipe = parse_handle(arguments[4]);
        const HANDLE response_pipe = parse_handle(arguments[5]);
        LocalFree(arguments);
        if (request_pipe == nullptr || response_pipe == nullptr ||
            !std::filesystem::is_regular_file(input_path) ||
            !std::filesystem::is_directory(cache_directory))
        {
            return 3;
        }
        return glance::office::run_word_preview_session(
            input_path,
            cache_directory,
            request_pipe,
            response_pipe);
    }

    if (argument_count != 3)
    {
        LocalFree(arguments);
        return 2;
    }

    const std::wstring input_path = arguments[1];
    const std::wstring output_path = arguments[2];
    LocalFree(arguments);

    if (!std::filesystem::is_regular_file(input_path) ||
        _wcsicmp(std::filesystem::path(output_path).extension().c_str(), L".pdf") != 0)
    {
        return 3;
    }
    DeleteFileW(output_path.c_str());
    return glance::office::export_to_pdf(input_path, output_path);
}

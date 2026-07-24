#include "../include/office_automation.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <filesystem>
#include <string>

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

    if (argument_count != 4)
    {
        LocalFree(arguments);
        return 2;
    }

    const std::wstring input_path = arguments[1];
    const std::wstring output_path = arguments[2];
    const HANDLE cancellation_event = parse_handle(arguments[3]);
    LocalFree(arguments);

    if (!std::filesystem::is_regular_file(input_path) ||
        _wcsicmp(std::filesystem::path(output_path).extension().c_str(), L".pdf") != 0 ||
        cancellation_event == nullptr)
    {
        return 3;
    }
    DeleteFileW(output_path.c_str());
    return glance::office::export_to_pdf(
        input_path,
        output_path,
        cancellation_event);
}

#include "../include/office_automation.h"

#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argument_count{};
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (arguments == nullptr || argument_count != 3)
    {
        if (arguments != nullptr)
        {
            LocalFree(arguments);
        }
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

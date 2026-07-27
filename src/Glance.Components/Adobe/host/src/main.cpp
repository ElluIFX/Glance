#include "../include/psd_decoder.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <filesystem>
#include <cstdint>
#include <string>
#include <string_view>

namespace
{
    struct Arguments
    {
        std::wstring kind;
        std::filesystem::path input;
        std::filesystem::path output;
        std::uint32_t maximum_dimension{};
    };

    bool parse_arguments(int argument_count, wchar_t** arguments, Arguments& result)
    {
        for (int index = 1; index + 1 < argument_count; index += 2)
        {
            const std::wstring_view name{ arguments[index] };
            if (name == L"--kind")
            {
                result.kind = arguments[index + 1];
            }
            else if (name == L"--input")
            {
                result.input = arguments[index + 1];
            }
            else if (name == L"--output")
            {
                result.output = arguments[index + 1];
            }
            else if (name == L"--maximum-dimension")
            {
                wchar_t* end{};
                const auto value = wcstoul(arguments[index + 1], &end, 10);
                if (end == arguments[index + 1] || *end != L'\0')
                {
                    return false;
                }
                result.maximum_dimension = value;
            }
            else
            {
                return false;
            }
        }
        return argument_count == 9 &&
            result.kind == L"psd" &&
            !result.input.empty() &&
            !result.output.empty() &&
            (result.maximum_dimension == 1024 ||
             result.maximum_dimension == 2048 ||
             result.maximum_dimension == 4096 ||
             result.maximum_dimension == 8192);
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argument_count{};
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (arguments == nullptr)
    {
        return ERROR_INVALID_PARAMETER;
    }

    Arguments parsed;
    const bool valid = parse_arguments(argument_count, arguments, parsed);
    LocalFree(arguments);
    if (!valid || !std::filesystem::is_regular_file(parsed.input))
    {
        return ERROR_INVALID_PARAMETER;
    }

    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized))
    {
        return static_cast<int>(initialized);
    }
    const bool success =
        glance::components::adobe::prepare_psd_preview(
            parsed.input,
            parsed.output,
            parsed.maximum_dimension) &&
        std::filesystem::is_regular_file(parsed.output);
    CoUninitialize();
    return success ? ERROR_SUCCESS : ERROR_GEN_FAILURE;
}

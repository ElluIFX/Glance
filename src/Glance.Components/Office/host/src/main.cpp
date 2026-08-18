#include "../include/preview_handler_host.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <cstdlib>
namespace
{
    HANDLE parse_handle(const wchar_t* value)
    {
        wchar_t* end{};
        const auto numeric = wcstoull(value, &end, 10);
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

    const HANDLE request_pipe = parse_handle(arguments[1]);
    const HANDLE response_pipe = parse_handle(arguments[2]);
    const HANDLE cancellation_event = parse_handle(arguments[3]);
    LocalFree(arguments);

    if (request_pipe == nullptr || response_pipe == nullptr ||
        cancellation_event == nullptr)
    {
        return 3;
    }
    return glance::office::run_preview_handler_host(
        request_pipe,
        response_pipe,
        cancellation_event);
}

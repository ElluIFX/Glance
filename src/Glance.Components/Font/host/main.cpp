#include "engine.h"
#include <algorithm>
#include <shellapi.h>
#include <winrt/base.h>

namespace
{
bool transfer(HANDLE pipe, void *data, std::size_t length, bool write)
{
    auto *bytes = static_cast<std::byte *>(data);
    while (length)
    {
        DWORD count{};
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(length, 65536));
        if (!(write ? WriteFile(pipe, bytes, chunk, &count, nullptr)
                    : ReadFile(pipe, bytes, chunk, &count, nullptr)) ||
            !count)
            return false;
        length -= count;
        bytes += count;
    }
    return true;
}
} // namespace
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int count{};
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments && count == 4 &&
        (wcscmp(arguments[1], L"--install-user") == 0 || wcscmp(arguments[1], L"--install-system") == 0))
    {
        const bool system = wcscmp(arguments[1], L"--install-system") == 0;
        const std::wstring file = arguments[2];
        const auto owner = reinterpret_cast<HWND>(_wcstoui64(arguments[3], nullptr, 10));
        LocalFree(arguments);
        try
        {
            winrt::init_apartment(winrt::apartment_type::single_threaded);
            SHELLEXECUTEINFOW execute{sizeof(execute)};
            execute.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
            execute.hwnd = IsWindow(owner) ? owner : nullptr;
            execute.lpVerb = system ? L"installAllUsers" : L"install";
            execute.lpFile = file.c_str();
            execute.nShow = SW_SHOWNORMAL;
            return ShellExecuteExW(&execute) ? 0 : static_cast<int>(GetLastError());
        }
        catch (...) { return ERROR_FUNCTION_FAILED; }
    }
    if (!arguments || count != 4)
    {
        if (arguments)
            LocalFree(arguments);
        return 1;
    }
    winrt::handle input(reinterpret_cast<HANDLE>(_wcstoui64(arguments[1], nullptr, 10)));
    winrt::handle output(reinterpret_cast<HANDLE>(_wcstoui64(arguments[2], nullptr, 10)));
    winrt::handle mapping(reinterpret_cast<HANDLE>(_wcstoui64(arguments[3], nullptr, 10)));
    LocalFree(arguments);
    auto *pixels = MapViewOfFile(mapping.get(), FILE_MAP_WRITE, 0, 0, 0);
    if (!pixels)
        return 1;
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try
    {
        unsigned length{};
        if (!transfer(input.get(), &length, sizeof(length), false) || !length || length >= 32768)
            return 2;
        std::wstring path(length, L'\0');
        if (!transfer(input.get(), path.data(), length * 2, false))
            return 2;
        glance::font::Engine engine(path);
        glance::font::Request request;
        while (transfer(input.get(), &request, sizeof(request), false))
        {
            try
            {
                if (request.operation == glance::font::Operation::metadata)
                {
                    auto data = engine.metadata(request.face);
                    glance::font::Response response;
                    response.bytes = sizeof(data);
                    if (!transfer(output.get(), &response, sizeof(response), true) ||
                        !transfer(output.get(), &data, sizeof(data), true))
                        break;
                }
                else if (request.operation == glance::font::Operation::render)
                {
                    auto data = engine.render(request);
                    if (!VirtualAlloc(pixels, data.pixels.size(), MEM_COMMIT, PAGE_READWRITE))
                        winrt::throw_last_error();
                    memcpy(pixels, data.pixels.data(), data.pixels.size());
                    if (!transfer(output.get(), &data.info, sizeof(data.info), true))
                        break;
                }
                else
                    break;
            }
            catch (...)
            {
                glance::font::Response response;
                response.error = 1;
                if (!transfer(output.get(), &response, sizeof(response), true))
                    break;
            }
        }
    }
    catch (...)
    {
        glance::font::Response response;
        response.error = 1;
        transfer(output.get(), &response, sizeof(response), true);
        return 3;
    }
    return 0;
}

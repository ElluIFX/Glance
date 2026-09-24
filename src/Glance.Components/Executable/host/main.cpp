#include "viewer.h"
#include "signature.h"
#include <commctrl.h>
#include <shellapi.h>
#include <objbase.h>
#include <gdiplus.h>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace
{
    using namespace glance::contracts::native_preview;
    constexpr UINT request_message = WM_APP + 20;
    bool transfer(HANDLE handle, void* data, std::size_t length, bool write)
    {
        auto* cursor = static_cast<std::byte*>(data);
        while (length)
        {
            DWORD count{};
            const auto size = static_cast<DWORD>(std::min<std::size_t>(length, MAXDWORD));
            const bool success = write ? WriteFile(handle, cursor, size, &count, nullptr)
                                       : ReadFile(handle, cursor, size, &count, nullptr);
            if (!success || !count)
                return false;
            cursor += count;
            length -= count;
        }
        return true;
    }
    struct Request
    {
        RequestHeader header;
        std::vector<std::byte> data;
    };
    Status handle(const Request& request, glance::executable::Viewer& viewer)
    {
        switch (request.header.command)
        {
        case Command::open_document: {
            if (request.data.size() < sizeof(OpenRequest))
                return Status::invalid_request;
            OpenRequest open{};
            memcpy(&open, request.data.data(), sizeof(open));
            if (std::uint64_t(open.path_characters) * 2 != request.data.size() - sizeof(open))
                return Status::invalid_request;
            const std::wstring path(reinterpret_cast<const wchar_t*>(request.data.data() + sizeof(open)),
                                    open.path_characters);
            const RECT bounds{open.bounds.left, open.bounds.top, open.bounds.right, open.bounds.bottom};
            return viewer.open(reinterpret_cast<HWND>(open.parent_window), path, bounds, open.dpi,
                               open.visuals)
                       ? Status::success
                       : Status::open_failed;
        }
        case Command::resize: {
            if (request.data.size() != sizeof(ResizeRequest))
                return Status::invalid_request;
            ResizeRequest resize{};
            memcpy(&resize, request.data.data(), sizeof(resize));
            viewer.resize({resize.bounds.left, resize.bounds.top, resize.bounds.right, resize.bounds.bottom},
                          resize.dpi);
            return Status::success;
        }
        case Command::set_visuals: {
            if (request.data.size() != sizeof(PreviewVisuals))
                return Status::invalid_request;
            PreviewVisuals visuals{};
            memcpy(&visuals, request.data.data(), sizeof(visuals));
            viewer.visuals(visuals);
            return Status::success;
        }
        case Command::set_language:
            if (request.data.empty() || request.data.size() % 2 || request.data.size() > 170)
                return Status::invalid_request;
            viewer.language({reinterpret_cast<const wchar_t*>(request.data.data()), request.data.size() / 2});
            return Status::success;
        case Command::unload:
        case Command::shutdown:
            viewer.close();
            return Status::success;
        case Command::query_content_size:
            return Status::success;
        default:
            return Status::invalid_request;
        }
    }
} // namespace
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int count{};
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments || count != 4)
    {
        if (arguments)
            LocalFree(arguments);
        return 2;
    }
#ifdef _DEBUG
    if (wcscmp(arguments[1], L"--layout") == 0)
    {
        const std::wstring path = arguments[2], language = arguments[3];
        LocalFree(arguments);
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
            return 3;
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token{};
        Gdiplus::GdiplusStartup(&token, &input, nullptr);
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);
        {
            glance::executable::Viewer viewer;
            WNDCLASSW klass{};
            klass.hInstance = GetModuleHandleW(nullptr);
            klass.lpszClassName = L"Glance.Executable.Layout";
            klass.lpfnWndProc = [](HWND window, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT {
                if (message == WM_NCCREATE)
                    SetWindowLongPtrW(
                        window, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
                auto* target =
                    reinterpret_cast<glance::executable::Viewer*>(GetWindowLongPtrW(window, GWLP_USERDATA));
                if (message == WM_SIZE && target)
                {
                    RECT bounds{};
                    GetClientRect(window, &bounds);
                    target->resize(bounds, GetDpiForWindow(window));
                }
                if (message == WM_DESTROY)
                {
                    PostQuitMessage(0);
                    return 0;
                }
                return DefWindowProcW(window, message, wparam, lparam);
            };
            RegisterClassW(&klass);
            const auto window = CreateWindowW(klass.lpszClassName, L"Executable information — layout check",
                                              WS_OVERLAPPEDWINDOW, 100, 100, 1100, 800, nullptr, nullptr,
                                              klass.hInstance, &viewer);
            viewer.language(language);
            RECT bounds{};
            GetClientRect(window, &bounds);
            viewer.open(window, path, bounds, GetDpiForWindow(window), {0x00fafafa, 0x00191919, 0});
            ShowWindow(window, SW_SHOWNORMAL);
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0)
                if (!viewer.key(message))
                {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
        }
        Gdiplus::GdiplusShutdown(token);
        CoUninitialize();
        return 0;
    }
#endif
    if (wcscmp(arguments[1], L"--signature") == 0)
    {
        const auto result = glance::executable::signature_worker(
            arguments[3], reinterpret_cast<HANDLE>(_wcstoui64(arguments[2], nullptr, 10)));
        LocalFree(arguments);
        return result;
    }
    const HANDLE input = reinterpret_cast<HANDLE>(_wcstoui64(arguments[1], nullptr, 10));
    const HANDLE output = reinterpret_cast<HANDLE>(_wcstoui64(arguments[2], nullptr, 10));
    const HANDLE cancellation = reinterpret_cast<HANDLE>(_wcstoui64(arguments[3], nullptr, 10));
    LocalFree(arguments);
    if (!input || !output || !cancellation)
        return 2;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
        return 3;
    Gdiplus::GdiplusStartupInput gdiplus_input;
    ULONG_PTR gdiplus_token{};
    if (Gdiplus::GdiplusStartup(&gdiplus_token, &gdiplus_input, nullptr) != Gdiplus::Ok)
    {
        CoUninitialize();
        return 3;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    const auto thread = GetCurrentThreadId();
    std::mutex mutex;
    std::deque<Request> requests;
    std::atomic_bool stopped{};
    std::thread reader([&] {
        while (!stopped)
        {
            Request request;
            if (!transfer(input, &request.header, sizeof(request.header), false) ||
                request.header.magic != protocol_magic || request.header.version != protocol_version ||
                request.header.payload_size > maximum_payload_size)
                break;
            request.data.resize(request.header.payload_size);
            if (!transfer(input, request.data.data(), request.data.size(), false))
                break;
            {
                std::scoped_lock lock(mutex);
                requests.push_back(std::move(request));
            }
            PostThreadMessageW(thread, request_message, 0, 0);
        }
        PostThreadMessageW(thread, WM_QUIT, 0, 0);
    });
    std::thread canceller([&] {
        WaitForSingleObject(cancellation, INFINITE);
        if (!stopped)
            PostThreadMessageW(thread, WM_QUIT, 0, 0);
    });
    int exit_code{};
    try
    {
        glance::executable::Viewer viewer;
        bool running = true;
        while (running && GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            if (message.message != request_message)
            {
                if (!viewer.key(message))
                {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                continue;
            }
            std::deque<Request> pending;
            {
                std::scoped_lock lock(mutex);
                pending.swap(requests);
            }
            for (const auto& request : pending)
            {
                Status status{Status::preview_failed};
                try
                {
                    status = handle(request, viewer);
                }
                catch (...)
                {
                }
                const bool size =
                    status == Status::success && request.header.command == Command::query_content_size;
                ResponseHeader response{.status = status, .payload_size = size ? sizeof(ContentSize) : 0};
                ContentSize content{960, 680};
                if (!transfer(output, &response, sizeof(response), true) ||
                    (size && !transfer(output, &content, sizeof(content), true)))
                    running = false;
                if (request.header.command == Command::shutdown)
                    running = false;
            }
        }
    }
    catch (...)
    {
        exit_code = 4;
    }
    stopped = true;
    CancelSynchronousIo(reader.native_handle());
    SetEvent(cancellation);
    reader.join();
    canceller.join();
    CloseHandle(input);
    CloseHandle(output);
    CloseHandle(cancellation);
    Gdiplus::GdiplusShutdown(gdiplus_token);
    CoUninitialize();
    return exit_code;
}

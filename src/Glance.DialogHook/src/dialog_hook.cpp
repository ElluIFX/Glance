#include "dialog_hook_api.h"

#include <shellapi.h>
#include <shlobj_core.h>
#include <shobjidl_core.h>
#include <wrl/client.h>

#include <array>
#include <cwchar>
#include <string>
#include <string_view>

namespace
{
    using Microsoft::WRL::ComPtr;

    constexpr LONG result_empty = 0;
    constexpr LONG result_ready = 1;
    constexpr std::size_t path_capacity = 32768;

    struct SharedSelection
    {
        volatile LONG state{};
        DWORD process_id{};
        DWORD thread_id{};
        wchar_t path[path_capacity]{};
    };

#pragma data_seg(push, shared_data, ".glshare")
    SharedSelection shared_selection;
#pragma data_seg(pop, shared_data)
#pragma comment(linker, "/SECTION:.glshare,RWS")

    std::wstring_view window_class_name(HWND window, std::array<wchar_t, 128>& buffer)
    {
        const int length = GetClassNameW(window, buffer.data(), static_cast<int>(buffer.size()));
        return length > 0
            ? std::wstring_view(buffer.data(), static_cast<std::size_t>(length))
            : std::wstring_view{};
    }

    HWND find_dialog_window(HWND root)
    {
        struct SearchContext
        {
            HWND root{};
            HWND result{};
        } context{ root };

        EnumChildWindows(root, [](HWND window, LPARAM parameter) -> BOOL {
            auto& search = *reinterpret_cast<SearchContext*>(parameter);
            std::array<wchar_t, 128> class_buffer{};
            if (_wcsicmp(std::wstring(window_class_name(window, class_buffer)).c_str(), L"SHELLDLL_DefView") != 0)
            {
                return TRUE;
            }

            for (HWND current = window; current != nullptr; current = GetParent(current))
            {
                if (_wcsicmp(std::wstring(window_class_name(current, class_buffer)).c_str(), L"#32770") == 0)
                {
                    search.result = current;
                    return FALSE;
                }
                if (current == search.root)
                {
                    break;
                }
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&context));
        return context.result != nullptr ? context.result : root;
    }

    void write_selected_path(HWND query_window)
    {
        const auto shell_browser = reinterpret_cast<IShellBrowser*>(
            SendMessageW(find_dialog_window(query_window), WM_USER + 7, 0, 0));
        if (shell_browser == nullptr)
        {
            return;
        }

        ComPtr<IShellView> shell_view;
        ComPtr<IDataObject> selected_items;
        if (FAILED(shell_browser->QueryActiveShellView(&shell_view)) ||
            shell_view == nullptr ||
            FAILED(shell_view->GetItemObject(SVGIO_SELECTION, IID_PPV_ARGS(&selected_items))) ||
            selected_items == nullptr)
        {
            return;
        }

        FORMATETC format{
            static_cast<CLIPFORMAT>(CF_HDROP),
            nullptr,
            DVASPECT_CONTENT,
            -1,
            TYMED_HGLOBAL
        };
        STGMEDIUM storage{};
        if (FAILED(selected_items->GetData(&format, &storage)))
        {
            return;
        }

        const auto drop = static_cast<HDROP>(GlobalLock(storage.hGlobal));
        if (drop != nullptr && DragQueryFileW(drop, 0, shared_selection.path, path_capacity) > 0)
        {
            MemoryBarrier();
            InterlockedExchange(&shared_selection.state, result_ready);
        }
        if (drop != nullptr)
        {
            GlobalUnlock(storage.hGlobal);
        }
        ReleaseStgMedium(&storage);
    }
}

extern "C" __declspec(dllexport) void WINAPI GlanceDialogHookPrepare(
    DWORD process_id,
    DWORD thread_id)
{
    InterlockedExchange(&shared_selection.state, result_empty);
    shared_selection.path[0] = L'\0';
    shared_selection.process_id = process_id;
    shared_selection.thread_id = thread_id;
    MemoryBarrier();
}

extern "C" __declspec(dllexport) UINT WINAPI GlanceDialogHookReadPath(
    PWSTR buffer,
    UINT capacity)
{
    if (buffer == nullptr || capacity == 0 ||
        InterlockedCompareExchange(&shared_selection.state, result_empty, result_empty) != result_ready)
    {
        return 0;
    }

    MemoryBarrier();
    const std::size_t length = wcsnlen_s(shared_selection.path, path_capacity);
    if (length == 0 || length >= capacity)
    {
        return 0;
    }
    if (wmemcpy_s(buffer, capacity, shared_selection.path, length + 1) != 0)
    {
        return 0;
    }
    return static_cast<UINT>(length);
}

extern "C" __declspec(dllexport) LRESULT CALLBACK GlanceDialogHookProc(
    int code,
    WPARAM wparam,
    LPARAM lparam)
{
    if (code >= 0 && lparam != 0)
    {
        const auto* message = reinterpret_cast<const CWPSTRUCT*>(lparam);
        static const UINT query_message = RegisterWindowMessageW(glance::dialog_hook::query_message_name);
        if (query_message != 0 &&
            message->message == query_message &&
            shared_selection.process_id == GetCurrentProcessId() &&
            shared_selection.thread_id == GetCurrentThreadId())
        {
            write_selected_path(message->hwnd);
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

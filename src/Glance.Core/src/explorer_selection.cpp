#include "explorer_selection.h"

#include <windows.h>
#include <oaidl.h>
#include <ocidl.h>
#include <exdisp.h>
#include <servprov.h>
#include <shlguid.h>
#include <shobjidl_core.h>
#include <uiautomationclient.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    using Microsoft::WRL::ComPtr;

    std::wstring process_image_name(DWORD process_id)
    {
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
        if (process == nullptr)
        {
            return {};
        }

        std::wstring path(32768, L'\0');
        DWORD size = static_cast<DWORD>(path.size());
        if (!QueryFullProcessImageNameW(process, 0, path.data(), &size))
        {
            CloseHandle(process);
            return {};
        }
        CloseHandle(process);
        path.resize(size);
        return path;
    }

    std::wstring window_class_name(HWND window)
    {
        wchar_t name[128]{};
        GetClassNameW(window, name, static_cast<int>(std::size(name)));
        return name;
    }

    std::wstring shell_item_name(IShellItem* item, SIGDN kind)
    {
        PWSTR value{};
        if (FAILED(item->GetDisplayName(kind, &value)))
        {
            return {};
        }
        std::wstring result(value);
        CoTaskMemFree(value);
        return result;
    }

    glance::contracts::FileDescriptor describe_item(IShellItem* item)
    {
        glance::contracts::FileDescriptor result;
        result.display_name = shell_item_name(item, SIGDN_NORMALDISPLAY);
        result.filesystem_path = shell_item_name(item, SIGDN_FILESYSPATH);
        result.shell_parsing_name = shell_item_name(item, SIGDN_DESKTOPABSOLUTEPARSING);
        result.is_filesystem = !result.filesystem_path.empty();

        if (result.is_filesystem)
        {
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (GetFileAttributesExW(result.filesystem_path.c_str(), GetFileExInfoStandard, &attributes))
            {
                result.attributes = attributes.dwFileAttributes;
                result.size = (static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32U) |
                              attributes.nFileSizeLow;
                result.last_write_time = (static_cast<std::uint64_t>(attributes.ftLastWriteTime.dwHighDateTime) << 32U) |
                                         attributes.ftLastWriteTime.dwLowDateTime;
                result.is_cloud_placeholder =
                    (attributes.dwFileAttributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS |
                                                     FILE_ATTRIBUTE_RECALL_ON_OPEN)) != 0;
                result.is_hydrated = !result.is_cloud_placeholder;
            }
        }
        return result;
    }
}

namespace glance::core
{
    ExplorerSelectionService::ExplorerSelectionService() = default;
    ExplorerSelectionService::~ExplorerSelectionService() = default;

    bool ExplorerSelectionService::is_text_input_focused(const GUITHREADINFO& thread_info) const
    {
        if (thread_info.hwndCaret != nullptr || (thread_info.flags & GUI_CARETBLINKING) != 0)
        {
            return true;
        }

        if (automation_ == nullptr)
        {
            if (FAILED(CoCreateInstance(
                    CLSID_CUIAutomation8,
                    nullptr,
                    CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(&automation_))))
            {
                return true;
            }
            static_cast<void>(automation_->put_ConnectionTimeout(100));
            static_cast<void>(automation_->put_TransactionTimeout(100));
        }

        ComPtr<IUIAutomationElement> focused;
        CONTROLTYPEID control_type{};
        if (FAILED(automation_->GetFocusedElement(&focused)) ||
            focused == nullptr ||
            FAILED(focused->get_CurrentControlType(&control_type)))
        {
            return true;
        }
        return control_type == UIA_EditControlTypeId || control_type == UIA_DocumentControlTypeId;
    }

    bool ExplorerSelectionService::is_explorer_window(HWND window, DWORD& process_id)
    {
        GetWindowThreadProcessId(window, &process_id);
        auto path = process_image_name(process_id);
        if (path.empty())
        {
            return false;
        }
        auto name = std::filesystem::path(path).filename().wstring();
        std::ranges::transform(name, name.begin(), [](wchar_t value) { return std::towlower(value); });
        return name == L"explorer.exe";
    }

    glance::contracts::SelectionSnapshot ExplorerSelectionService::query_foreground() const
    {
        glance::contracts::SelectionSnapshot snapshot;
        snapshot.timestamp_ms = GetTickCount64();

        HWND foreground = GetForegroundWindow();
        HWND root = GetAncestor(foreground, GA_ROOT);
        DWORD process_id{};
        if (root == nullptr || !is_explorer_window(root, process_id))
        {
            return snapshot;
        }

        snapshot.source_window = reinterpret_cast<std::uintptr_t>(root);
        snapshot.source_process_id = process_id;
        snapshot.host_kind = glance::contracts::HostKind::explorer;

        ComPtr<IShellWindows> shell_windows;
        if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&shell_windows))))
        {
            return snapshot;
        }

        long count{};
        if (FAILED(shell_windows->get_Count(&count)))
        {
            return snapshot;
        }

        for (long index = 0; index < count; ++index)
        {
            VARIANT item_index{};
            VariantInit(&item_index);
            item_index.vt = VT_I4;
            item_index.lVal = index;

            ComPtr<IDispatch> dispatch;
            if (FAILED(shell_windows->Item(item_index, &dispatch)) || dispatch == nullptr)
            {
                continue;
            }

            ComPtr<IWebBrowserApp> browser;
            if (FAILED(dispatch.As(&browser)))
            {
                continue;
            }

            SHANDLE_PTR browser_handle{};
            if (FAILED(browser->get_HWND(&browser_handle)) ||
                GetAncestor(reinterpret_cast<HWND>(browser_handle), GA_ROOT) != root)
            {
                continue;
            }

            ComPtr<IServiceProvider> service_provider;
            ComPtr<IShellBrowser> shell_browser;
            ComPtr<IShellView> shell_view;
            ComPtr<IFolderView2> folder_view;
            if (FAILED(browser.As(&service_provider)) ||
                FAILED(service_provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&shell_browser))) ||
                FAILED(shell_browser->QueryActiveShellView(&shell_view)) ||
                FAILED(shell_view.As(&folder_view)))
            {
                continue;
            }

            HWND view_window{};
            if (FAILED(shell_view->GetWindow(&view_window)) || view_window == nullptr)
            {
                continue;
            }

            GUITHREADINFO thread_info{ sizeof(GUITHREADINFO) };
            if (!GetGUIThreadInfo(0, &thread_info))
            {
                continue;
            }
            const bool focused_view = thread_info.hwndFocus == view_window ||
                (thread_info.hwndFocus != nullptr && IsChild(view_window, thread_info.hwndFocus));
            const bool input_site = thread_info.hwndFocus != nullptr &&
                GetAncestor(thread_info.hwndFocus, GA_ROOT) == root &&
                _wcsicmp(window_class_name(thread_info.hwndFocus).c_str(), L"InputSiteWindowClass") == 0;
            if (!focused_view && (!input_site || is_text_input_focused(thread_info)))
            {
                continue;
            }
            snapshot.accepts_hotkey = true;

            ComPtr<IShellItemArray> selected_items;
            if (FAILED(folder_view->Items(SVGIO_SELECTION, IID_PPV_ARGS(&selected_items))) || selected_items == nullptr)
            {
                return snapshot;
            }

            DWORD selected_count{};
            if (FAILED(selected_items->GetCount(&selected_count)))
            {
                return snapshot;
            }

            snapshot.items.reserve(selected_count);
            for (DWORD selected_index = 0; selected_index < selected_count; ++selected_index)
            {
                ComPtr<IShellItem> selected_item;
                if (SUCCEEDED(selected_items->GetItemAt(selected_index, &selected_item)))
                {
                    snapshot.items.push_back(describe_item(selected_item.Get()));
                }
            }

            int focused_view_index{-1};
            if (SUCCEEDED(folder_view->GetFocusedItem(&focused_view_index)) && focused_view_index >= 0)
            {
                ComPtr<IShellFolder> folder;
                ComPtr<IShellItem> focused_item;
                PITEMID_CHILD focused_id{};
                if (SUCCEEDED(folder_view->GetFolder(IID_PPV_ARGS(&folder))) &&
                    SUCCEEDED(folder_view->Item(focused_view_index, &focused_id)) &&
                    focused_id != nullptr &&
                    SUCCEEDED(SHCreateItemWithParent(
                        nullptr,
                        folder.Get(),
                        focused_id,
                        IID_PPV_ARGS(&focused_item))))
                {
                    const auto focused_path = shell_item_name(focused_item.Get(), SIGDN_FILESYSPATH);
                    for (std::size_t selected_index = 0; selected_index < snapshot.items.size(); ++selected_index)
                    {
                        if (_wcsicmp(snapshot.items[selected_index].filesystem_path.c_str(), focused_path.c_str()) == 0)
                        {
                            snapshot.focused_index = static_cast<std::uint32_t>(selected_index);
                            break;
                        }
                    }
                }
                CoTaskMemFree(focused_id);
            }
            return snapshot;
        }
        return snapshot;
    }
}

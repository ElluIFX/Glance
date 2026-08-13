#include "explorer_selection.h"
#include "glance/contracts/diagnostics.h"

#include <windows.h>
#include <commdlg.h>
#include <oaidl.h>
#include <ocidl.h>
#include <exdisp.h>
#include <oleacc.h>
#include <servprov.h>
#include <shlguid.h>
#include <shlobj_core.h>
#include <shobjidl_core.h>
#include <shldisp.h>
#include <uiautomationclient.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
    using Microsoft::WRL::ComPtr;

    std::wstring process_executable_name(DWORD process_id)
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
        const auto separator = path.find_last_of(L"\\/");
        if (separator != std::wstring::npos)
        {
            path.erase(0, separator + 1);
        }
        std::ranges::transform(path, path.begin(), [](wchar_t value) { return std::towlower(value); });
        return path;
    }

    std::wstring window_class_name(HWND window)
    {
        wchar_t name[128]{};
        GetClassNameW(window, name, static_cast<int>(std::size(name)));
        return name;
    }

    bool shares_focus_scope(HWND view_window, HWND focused_window)
    {
        return focused_window != nullptr &&
            (focused_window == view_window ||
             IsChild(view_window, focused_window) ||
             IsChild(focused_window, view_window));
    }

    HWND find_ancestor_window(HWND window, HWND boundary, std::wstring_view class_name)
    {
        for (HWND current = window; current != nullptr; current = GetParent(current))
        {
            if (_wcsicmp(window_class_name(current).c_str(), std::wstring(class_name).c_str()) == 0)
            {
                return current;
            }
            if (current == boundary)
            {
                break;
            }
        }
        return boundary;
    }

    bool is_native_text_input_focused(const GUITHREADINFO& thread_info)
    {
        const HWND focused = thread_info.hwndFocus;
        const HWND root = focused == nullptr ? nullptr : GetAncestor(focused, GA_ROOT);
        for (HWND current = focused; current != nullptr; current = GetParent(current))
        {
            const auto class_name = window_class_name(current);
            if (_wcsicmp(class_name.c_str(), L"Edit") == 0 ||
                _wcsnicmp(class_name.c_str(), L"RichEdit", 8) == 0)
            {
                return true;
            }
            if (current == root)
            {
                break;
            }
        }

        const bool blinking_caret = thread_info.hwndCaret != nullptr &&
            (thread_info.flags & GUI_CARETBLINKING) != 0;
        return blinking_caret && focused != nullptr &&
            (thread_info.hwndCaret == focused ||
             IsChild(focused, thread_info.hwndCaret) ||
             IsChild(thread_info.hwndCaret, focused));
    }

    void log_common_dialog_rejection(std::wstring_view reason)
    {
        static ULONGLONG previous_timestamp{};

        const ULONGLONG now = GetTickCount64();
        if (previous_timestamp == 0 || now - previous_timestamp >= 2000)
        {
            previous_timestamp = now;
            glance::contracts::log_event(L"Common dialog selection rejected: " + std::wstring(reason) + L".");
        }
    }

    void log_explorer_rejection(std::wstring_view reason)
    {
        static ULONGLONG previous_timestamp{};

        const ULONGLONG now = GetTickCount64();
        if (previous_timestamp == 0 || now - previous_timestamp >= 2000)
        {
            previous_timestamp = now;
            glance::contracts::log_event(L"Explorer selection rejected: " + std::wstring(reason) + L".");
        }
    }

    bool common_dialog_focus_allowed(
        HWND root,
        HWND view_window,
        const GUITHREADINFO& thread_info)
    {
        if (shares_focus_scope(view_window, thread_info.hwndFocus))
        {
            return true;
        }
        if (thread_info.hwndFocus == nullptr ||
            GetAncestor(thread_info.hwndFocus, GA_ROOT) != root)
        {
            return false;
        }

        const auto focus_class = window_class_name(thread_info.hwndFocus);
        return _wcsicmp(focus_class.c_str(), L"DirectUIHWND") == 0 ||
            _wcsicmp(focus_class.c_str(), L"DUIViewWndClassName") == 0;
    }

    HWND find_descendant_window(HWND root, std::wstring_view class_name, HWND focused_window)
    {
        struct SearchContext
        {
            std::wstring_view class_name;
            HWND focused_window{};
            HWND result{};
        } context{ class_name, focused_window };

        EnumChildWindows(root, [](HWND window, LPARAM parameter) -> BOOL {
            auto& search = *reinterpret_cast<SearchContext*>(parameter);
            if (_wcsicmp(window_class_name(window).c_str(), std::wstring(search.class_name).c_str()) == 0)
            {
                if (search.result == nullptr || IsWindowVisible(window))
                {
                    search.result = window;
                }
                if (IsWindowVisible(window) && shares_focus_scope(window, search.focused_window))
                {
                    return FALSE;
                }
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&context));
        return context.result;
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

        if (!result.is_filesystem)
        {
            PIDLIST_ABSOLUTE id_list{};
            if (SUCCEEDED(SHGetIDListFromObject(item, &id_list)) && id_list != nullptr)
            {
                const UINT size = ILGetSize(id_list);
                if (size > 0 && size <= 64U * 1024U)
                {
                    const auto* bytes = reinterpret_cast<const std::uint8_t*>(id_list);
                    result.shell_id_list.assign(bytes, bytes + size);
                }
                CoTaskMemFree(id_list);
            }

            SFGAOF shell_attributes{};
            if (SUCCEEDED(item->GetAttributes(SFGAO_FOLDER, &shell_attributes)) &&
                (shell_attributes & SFGAO_FOLDER) != 0)
            {
                result.attributes |= FILE_ATTRIBUTE_DIRECTORY;
            }
        }

        if (result.is_filesystem)
        {
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (GetFileAttributesExW(result.filesystem_path.c_str(), GetFileExInfoStandard, &attributes))
            {
                result.attributes = attributes.dwFileAttributes;
                result.size = (static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32U) |
                              attributes.nFileSizeLow;
                result.creation_time = (static_cast<std::uint64_t>(attributes.ftCreationTime.dwHighDateTime) << 32U) |
                                       attributes.ftCreationTime.dwLowDateTime;
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

    glance::contracts::FileDescriptor describe_path(std::wstring_view path)
    {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                std::wstring(path).c_str(),
                nullptr,
                IID_PPV_ARGS(&item))))
        {
            return describe_item(item.Get());
        }
        return {};
    }

    std::wstring common_dialog_file_path(HWND dialog_window)
    {
        std::vector<wchar_t> buffer(32768, L'\0');
        DWORD_PTR message_result{};
        if (SendMessageTimeoutW(
                dialog_window,
                CDM_GETFILEPATH,
                static_cast<WPARAM>(buffer.size()),
                reinterpret_cast<LPARAM>(buffer.data()),
                SMTO_ABORTIFHUNG | SMTO_BLOCK,
                50,
                &message_result) == 0 ||
            message_result <= 1 ||
            buffer.front() == L'\0')
        {
            return {};
        }
        return buffer.data();
    }

    bool same_item(
        const glance::contracts::FileDescriptor& descriptor,
        std::wstring_view filesystem_path,
        std::wstring_view parsing_name)
    {
        return (!filesystem_path.empty() &&
                _wcsicmp(descriptor.filesystem_path.c_str(), std::wstring(filesystem_path).c_str()) == 0) ||
               (!parsing_name.empty() &&
                _wcsicmp(descriptor.shell_parsing_name.c_str(), std::wstring(parsing_name).c_str()) == 0);
    }

    bool populate_from_folder_view(
        IFolderView2* folder_view,
        glance::contracts::SelectionSnapshot& snapshot)
    {
        ComPtr<IShellItemArray> selected_items;
        if (FAILED(folder_view->Items(SVGIO_SELECTION, IID_PPV_ARGS(&selected_items))) || selected_items == nullptr)
        {
            return false;
        }

        DWORD selected_count{};
        if (FAILED(selected_items->GetCount(&selected_count)))
        {
            return false;
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
                const auto focused_parsing_name = shell_item_name(focused_item.Get(), SIGDN_DESKTOPABSOLUTEPARSING);
                for (std::size_t selected_index = 0; selected_index < snapshot.items.size(); ++selected_index)
                {
                    if (same_item(snapshot.items[selected_index], focused_path, focused_parsing_name))
                    {
                        snapshot.focused_index = static_cast<std::uint32_t>(selected_index);
                        break;
                    }
                }
            }
            CoTaskMemFree(focused_id);
        }
        return true;
    }

    bool populate_from_shell_browser(
        IShellBrowser* shell_browser,
        glance::contracts::SelectionSnapshot& snapshot,
        HWND& view_window)
    {
        ComPtr<IShellView> shell_view;
        ComPtr<IFolderView2> folder_view;
        if (FAILED(shell_browser->QueryActiveShellView(&shell_view)) ||
            FAILED(shell_view.As(&folder_view)) ||
            FAILED(shell_view->GetWindow(&view_window)) ||
            view_window == nullptr)
        {
            return false;
        }
        return populate_from_folder_view(folder_view.Get(), snapshot);
    }

    std::wstring folder_view_path(IFolderView2* folder_view)
    {
        ComPtr<IShellFolder> folder;
        ComPtr<IPersistFolder2> persist_folder;
        PIDLIST_ABSOLUTE folder_id{};
        if (folder_view == nullptr ||
            FAILED(folder_view->GetFolder(IID_PPV_ARGS(&folder))) ||
            folder == nullptr ||
            FAILED(folder.As(&persist_folder)) ||
            FAILED(persist_folder->GetCurFolder(&folder_id)) ||
            folder_id == nullptr)
        {
            return {};
        }
        ComPtr<IShellItem> folder_item;
        const HRESULT item_result = SHCreateItemFromIDList(
            folder_id,
            IID_PPV_ARGS(&folder_item));
        CoTaskMemFree(folder_id);
        return SUCCEEDED(item_result) && folder_item != nullptr
            ? shell_item_name(folder_item.Get(), SIGDN_FILESYSPATH)
            : std::wstring{};
    }

    bool resolve_folder_view(
        HWND source_window,
        std::wstring_view expected_folder_path,
        HWND expected_view_window,
        ComPtr<IFolderView2>& folder_view,
        HWND* resolved_view_window = nullptr)
    {
        if (source_window == nullptr || !IsWindow(source_window))
        {
            return false;
        }

        ComPtr<IShellWindows> shell_windows;
        if (FAILED(CoCreateInstance(
                CLSID_ShellWindows,
                nullptr,
                CLSCTX_LOCAL_SERVER,
                IID_PPV_ARGS(&shell_windows))))
        {
            return false;
        }

        const auto class_name = window_class_name(source_window);
        const bool desktop = _wcsicmp(class_name.c_str(), L"Progman") == 0 ||
            _wcsicmp(class_name.c_str(), L"WorkerW") == 0;
        const auto folder_matches = [expected_folder_path](IFolderView2* candidate) {
            if (expected_folder_path.empty())
            {
                return true;
            }
            const auto path = folder_view_path(candidate);
            return !path.empty() &&
                _wcsicmp(path.c_str(), expected_folder_path.data()) == 0;
        };
        if (desktop)
        {
            VARIANT location{};
            VARIANT location_root{};
            long desktop_handle{};
            ComPtr<IDispatch> dispatch;
            if (FAILED(shell_windows->FindWindowSW(
                    &location,
                    &location_root,
                    SWC_DESKTOP,
                    &desktop_handle,
                    SWFO_NEEDDISPATCH,
                    &dispatch)) ||
                dispatch == nullptr)
            {
                return false;
            }
            ComPtr<IServiceProvider> service_provider;
            ComPtr<IShellBrowser> shell_browser;
            ComPtr<IShellView> shell_view;
            ComPtr<IFolderView2> candidate;
            HWND view_window{};
            const bool resolved = SUCCEEDED(dispatch.As(&service_provider)) &&
                (SUCCEEDED(service_provider->QueryService(
                     SID_STopLevelBrowser,
                     IID_PPV_ARGS(&shell_browser))) ||
                 SUCCEEDED(service_provider->QueryService(
                     IID_IShellBrowser,
                     IID_PPV_ARGS(&shell_browser)))) &&
                shell_browser != nullptr &&
                SUCCEEDED(shell_browser->QueryActiveShellView(&shell_view)) &&
                shell_view != nullptr &&
                SUCCEEDED(shell_view->GetWindow(&view_window)) &&
                SUCCEEDED(shell_view.As(&candidate)) &&
                (expected_view_window == nullptr || view_window == expected_view_window) &&
                folder_matches(candidate.Get());
            if (!resolved)
            {
                return false;
            }
            folder_view = std::move(candidate);
            if (resolved_view_window != nullptr)
            {
                *resolved_view_window = view_window;
            }
            return true;
        }

        GUITHREADINFO thread_info{ sizeof(thread_info) };
        DWORD source_process_id{};
        const DWORD source_thread_id = GetWindowThreadProcessId(
            source_window,
            &source_process_id);
        static_cast<void>(source_process_id);
        static_cast<void>(GetGUIThreadInfo(source_thread_id, &thread_info));
        ComPtr<IFolderView2> fallback;
        HWND fallback_view_window{};
        long count{};
        if (FAILED(shell_windows->get_Count(&count)))
        {
            return false;
        }
        for (long index = 0; index < count; ++index)
        {
            VARIANT item_index{};
            item_index.vt = VT_I4;
            item_index.lVal = index;
            ComPtr<IDispatch> dispatch;
            ComPtr<IWebBrowserApp> browser;
            SHANDLE_PTR browser_handle{};
            if (FAILED(shell_windows->Item(item_index, &dispatch)) ||
                dispatch == nullptr ||
                FAILED(dispatch.As(&browser)) ||
                FAILED(browser->get_HWND(&browser_handle)) ||
                GetAncestor(reinterpret_cast<HWND>(browser_handle), GA_ROOT) != source_window)
            {
                continue;
            }
            ComPtr<IServiceProvider> service_provider;
            ComPtr<IShellBrowser> shell_browser;
            ComPtr<IShellView> shell_view;
            ComPtr<IFolderView2> candidate;
            HWND view_window{};
            if (SUCCEEDED(browser.As(&service_provider)) &&
                SUCCEEDED(service_provider->QueryService(
                    SID_STopLevelBrowser,
                    IID_PPV_ARGS(&shell_browser))) &&
                shell_browser != nullptr &&
                SUCCEEDED(shell_browser->QueryActiveShellView(&shell_view)) &&
                shell_view != nullptr &&
                SUCCEEDED(shell_view->GetWindow(&view_window)) &&
                SUCCEEDED(shell_view.As(&candidate)) &&
                (expected_view_window == nullptr || view_window == expected_view_window) &&
                folder_matches(candidate.Get()))
            {
                const bool focused = thread_info.hwndFocus == view_window ||
                    (thread_info.hwndFocus != nullptr &&
                     IsChild(view_window, thread_info.hwndFocus));
                if (expected_view_window != nullptr || focused)
                {
                    folder_view = std::move(candidate);
                    if (resolved_view_window != nullptr)
                    {
                        *resolved_view_window = view_window;
                    }
                    return true;
                }
                if (fallback == nullptr)
                {
                    fallback = std::move(candidate);
                    fallback_view_window = view_window;
                }
            }
        }
        if (fallback == nullptr)
        {
            return false;
        }
        folder_view = std::move(fallback);
        if (resolved_view_window != nullptr)
        {
            *resolved_view_window = fallback_view_window;
        }
        return true;
    }

    glance::contracts::FileDescriptor describe_folder_item(FolderItem* item)
    {
        BSTR item_path{};
        BSTR item_name{};
        static_cast<void>(item->get_Path(&item_path));
        static_cast<void>(item->get_Name(&item_name));

        glance::contracts::FileDescriptor descriptor;
        if (item_path != nullptr)
        {
            ComPtr<IShellItem> shell_item;
            if (SUCCEEDED(SHCreateItemFromParsingName(item_path, nullptr, IID_PPV_ARGS(&shell_item))))
            {
                descriptor = describe_item(shell_item.Get());
            }
            else
            {
                descriptor.filesystem_path = item_path;
                descriptor.is_filesystem = GetFileAttributesW(item_path) != INVALID_FILE_ATTRIBUTES;
            }
        }
        if (descriptor.display_name.empty() && item_name != nullptr)
        {
            descriptor.display_name = item_name;
        }
        SysFreeString(item_path);
        SysFreeString(item_name);
        return descriptor;
    }

    bool populate_from_native_shell_view(
        HWND view_window,
        glance::contracts::SelectionSnapshot& snapshot)
    {
        ComPtr<IDispatch> dispatch;
        if (FAILED(AccessibleObjectFromWindow(
                view_window,
                static_cast<DWORD>(OBJID_NATIVEOM),
                IID_PPV_ARGS(&dispatch))) ||
            dispatch == nullptr)
        {
            return false;
        }

        ComPtr<IShellFolderViewDual> folder_view;
        ComPtr<FolderItems> selected_items;
        if (FAILED(dispatch.As(&folder_view)) ||
            FAILED(folder_view->SelectedItems(&selected_items)) ||
            selected_items == nullptr)
        {
            return false;
        }

        long selected_count{};
        if (FAILED(selected_items->get_Count(&selected_count)) || selected_count < 0)
        {
            return false;
        }
        snapshot.items.reserve(static_cast<std::size_t>(selected_count));
        for (long index = 0; index < selected_count; ++index)
        {
            VARIANT item_index{};
            VariantInit(&item_index);
            item_index.vt = VT_I4;
            item_index.lVal = index;
            ComPtr<FolderItem> item;
            if (SUCCEEDED(selected_items->Item(item_index, &item)) && item != nullptr)
            {
                snapshot.items.push_back(describe_folder_item(item.Get()));
            }
        }

        ComPtr<FolderItem> focused_item;
        if (SUCCEEDED(folder_view->get_FocusedItem(&focused_item)) && focused_item != nullptr)
        {
            const auto focused = describe_folder_item(focused_item.Get());
            for (std::size_t index = 0; index < snapshot.items.size(); ++index)
            {
                if (same_item(snapshot.items[index], focused.filesystem_path, focused.shell_parsing_name))
                {
                    snapshot.focused_index = static_cast<std::uint32_t>(index);
                    break;
                }
            }
        }
        return true;
    }
}

namespace glance::core
{
    ExplorerSelectionService::ExplorerSelectionService() = default;
    ExplorerSelectionService::~ExplorerSelectionService() = default;

    bool ExplorerSelectionService::is_text_input_focused() const
    {
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

    glance::contracts::SelectionSnapshot ExplorerSelectionService::query_foreground()
    {
        glance::contracts::SelectionSnapshot snapshot;
        snapshot.timestamp_ms = GetTickCount64();

        const HWND foreground = GetForegroundWindow();
        const HWND root = GetAncestor(foreground, GA_ROOT);
        DWORD process_id{};
        if (root == nullptr)
        {
            return snapshot;
        }

        const DWORD foreground_thread_id = GetWindowThreadProcessId(root, &process_id);
        const auto root_class = window_class_name(root);
        const bool desktop = _wcsicmp(root_class.c_str(), L"Progman") == 0 ||
            _wcsicmp(root_class.c_str(), L"WorkerW") == 0;

        GUITHREADINFO thread_info{ sizeof(GUITHREADINFO) };
        if (!GetGUIThreadInfo(foreground_thread_id, &thread_info))
        {
            return snapshot;
        }

        const auto process_name = process_executable_name(process_id);
        const bool explorer = process_name == L"explorer.exe";
        const HWND dialog_view = explorer
            ? nullptr
            : find_descendant_window(root, L"SHELLDLL_DefView", thread_info.hwndFocus);
        const bool common_dialog = dialog_view != nullptr;
        const auto external_selection = external_hosts_.query(ExternalHostContext{
            root,
            process_id,
            foreground_thread_id,
            thread_info,
            process_name,
            root_class,
        });
        if (!common_dialog)
        {
            dialog_hook_.detach();
            dialog_cache_window_ = nullptr;
            dialog_cache_timestamp_ = 0;
            dialog_cache_path_.clear();
        }
        if (!common_dialog && !explorer && !external_selection)
        {
            return snapshot;
        }

        snapshot.source_window = reinterpret_cast<std::uintptr_t>(root);
        snapshot.source_process_id = process_id;
        snapshot.host_kind = external_selection
            ? glance::contracts::HostKind::external_source
            : (common_dialog
                ? glance::contracts::HostKind::common_dialog
                : glance::contracts::HostKind::explorer);

        if (external_selection)
        {
            snapshot.source_id = external_selection->source_id;
            snapshot.source_capabilities = external_selection->capabilities;
            snapshot.text_input_active = external_selection->text_input_active;
            if (!external_selection->filesystem_path.empty())
            {
                auto descriptor = describe_path(external_selection->filesystem_path);
                if (descriptor.is_filesystem)
                {
                    snapshot.items.push_back(std::move(descriptor));
                }
            }
            snapshot.accepts_hotkey = external_selection->accepts_hotkey && !snapshot.items.empty();
            return snapshot;
        }

        if (common_dialog)
        {
            if (is_native_text_input_focused(thread_info))
            {
                snapshot.text_input_active = true;
                log_common_dialog_rejection(L"text input is focused");
                return snapshot;
            }

            if (!common_dialog_focus_allowed(root, dialog_view, thread_info))
            {
                log_common_dialog_rejection(L"focus is outside the Shell view (class " +
                    window_class_name(thread_info.hwndFocus) + L")");
                return snapshot;
            }

            const bool native_selection_available = populate_from_native_shell_view(dialog_view, snapshot);
            if (snapshot.items.empty())
            {
                const HWND dialog_window = find_ancestor_window(dialog_view, root, L"#32770");
                const auto path = common_dialog_file_path(dialog_window);
                if (!path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
                {
                    auto descriptor = describe_path(path);
                    if (descriptor.is_filesystem)
                    {
                        snapshot.items.push_back(std::move(descriptor));
                    }
                }
            }

            if (snapshot.items.empty())
            {
                const ULONGLONG now = GetTickCount64();
                if (dialog_cache_window_ != root)
                {
                    dialog_cache_window_ = root;
                    dialog_cache_timestamp_ = 0;
                    dialog_cache_path_.clear();
                }
                if (dialog_cache_timestamp_ == 0 || now - dialog_cache_timestamp_ >= 50)
                {
                    dialog_cache_path_ = dialog_hook_.query(root, process_id, foreground_thread_id);
                    dialog_cache_timestamp_ = GetTickCount64();
                }
                if (!dialog_cache_path_.empty() &&
                    GetFileAttributesW(dialog_cache_path_.c_str()) != INVALID_FILE_ATTRIBUTES)
                {
                    auto descriptor = describe_path(dialog_cache_path_);
                    if (descriptor.is_filesystem)
                    {
                        snapshot.items.push_back(std::move(descriptor));
                    }
                }
            }

            if (snapshot.items.empty())
            {
                log_common_dialog_rejection(native_selection_available
                    ? L"the Shell view has no selected filesystem item"
                    : L"the native object, CDM fallback, and dialog hook are unavailable");
                return snapshot;
            }
            snapshot.accepts_hotkey = !snapshot.items.empty();
            return snapshot;
        }

        if (is_native_text_input_focused(thread_info))
        {
            snapshot.text_input_active = true;
            log_explorer_rejection(L"native text input is focused");
            return snapshot;
        }

        const bool input_site = thread_info.hwndFocus != nullptr &&
            GetAncestor(thread_info.hwndFocus, GA_ROOT) == root &&
            _wcsicmp(window_class_name(thread_info.hwndFocus).c_str(), L"InputSiteWindowClass") == 0;
        if (input_site && is_text_input_focused())
        {
            snapshot.text_input_active = true;
            log_explorer_rejection(L"the InputSite focus resolves to text input");
            return snapshot;
        }

        ComPtr<IShellWindows> shell_windows;
        if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&shell_windows))))
        {
            log_explorer_rejection(L"IShellWindows is unavailable");
            return snapshot;
        }

        if (desktop)
        {
            VARIANT location{};
            VARIANT location_root{};
            VariantInit(&location);
            VariantInit(&location_root);
            long desktop_handle{};
            ComPtr<IDispatch> desktop_dispatch;
            if (SUCCEEDED(shell_windows->FindWindowSW(
                    &location,
                    &location_root,
                    SWC_DESKTOP,
                    &desktop_handle,
                    SWFO_NEEDDISPATCH,
                    &desktop_dispatch)) &&
                desktop_dispatch != nullptr)
            {
                ComPtr<IServiceProvider> service_provider;
                ComPtr<IShellBrowser> shell_browser;
                HWND view_window{};
                if (SUCCEEDED(desktop_dispatch.As(&service_provider)) &&
                    (SUCCEEDED(service_provider->QueryService(
                         SID_STopLevelBrowser, IID_PPV_ARGS(&shell_browser))) ||
                     SUCCEEDED(service_provider->QueryService(
                         IID_IShellBrowser, IID_PPV_ARGS(&shell_browser)))) &&
                    populate_from_shell_browser(shell_browser.Get(), snapshot, view_window))
                {
                    const bool focused_view = thread_info.hwndFocus == view_window ||
                        (thread_info.hwndFocus != nullptr && IsChild(view_window, thread_info.hwndFocus));
                    snapshot.accepts_hotkey = focused_view && !snapshot.items.empty();
                    if (!snapshot.accepts_hotkey)
                    {
                        log_explorer_rejection(snapshot.items.empty()
                            ? L"the desktop Shell view has no selected items"
                            : L"focus is outside the desktop Shell view");
                    }
                }
                else
                {
                    log_explorer_rejection(L"the desktop Shell view is unavailable");
                }
            }
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
            if (FAILED(browser.As(&service_provider)) ||
                FAILED(service_provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&shell_browser))) ||
                shell_browser == nullptr)
            {
                log_explorer_rejection(L"the top-level Shell browser is unavailable");
                continue;
            }

            HWND view_window{};
            glance::contracts::SelectionSnapshot selected_snapshot = snapshot;
            if (!populate_from_shell_browser(shell_browser.Get(), selected_snapshot, view_window))
            {
                log_explorer_rejection(L"the active Shell view is unavailable");
                continue;
            }

            const bool focused_view = thread_info.hwndFocus == view_window ||
                (thread_info.hwndFocus != nullptr && IsChild(view_window, thread_info.hwndFocus));
            if (!focused_view && !input_site)
            {
                log_explorer_rejection(L"focus is outside the active Shell view");
                continue;
            }
            selected_snapshot.accepts_hotkey = !selected_snapshot.items.empty();
            if (selected_snapshot.items.empty())
            {
                log_explorer_rejection(L"the active Shell view has no selected items");
            }
            return selected_snapshot;
        }
        log_explorer_rejection(L"no Shell browser matches the foreground window");
        return snapshot;
    }

    GalleryResponse ExplorerSelectionService::handle_gallery_command(
        const GalleryCommand& command,
        const std::function<bool()>& canceled,
        const std::function<void()>& report_progress)
    {
        GalleryResponse response;
        response.operation = command.operation;
        response.window_id = command.window_id;
        response.request_id = command.request_id;
        response.session_id = command.session_id;
        const auto fail = [&response](std::wstring error) {
            response.error = std::move(error);
            return response;
        };

        try
        {
            if (command.operation == GalleryOperation::close)
            {
                const auto session = gallery_sessions_.find(command.window_id);
                if (session != gallery_sessions_.end() &&
                    (command.session_id == 0 || session->second.id == command.session_id))
                {
                    gallery_sessions_.erase(session);
                }
                response.success = true;
                return response;
            }

            if (command.operation == GalleryOperation::open)
            {
                if (!command.source_id.empty())
                {
                    std::unordered_set<std::wstring> extensions;
                    for (auto extension : command.extensions)
                    {
                        std::ranges::transform(
                            extension,
                            extension.begin(),
                            [](wchar_t value) { return std::towlower(value); });
                        extensions.insert(std::move(extension));
                    }
                    if (extensions.empty())
                    {
                        return fail(L"unsupported");
                    }

                    std::uint32_t source_item_count{};
                    std::uint32_t focused_offset{};
                    std::uint64_t focused_item_id{};
                    if (!external_hosts_.query_item_count(
                            command.source_id,
                            command.source_window,
                            source_item_count,
                            focused_offset,
                            focused_item_id))
                    {
                        return fail(L"unavailable");
                    }

                    GallerySession session;
                    session.id = ++next_gallery_session_id_;
                    if (session.id == 0)
                    {
                        session.id = ++next_gallery_session_id_;
                    }
                    session.source_window = command.source_window;
                    session.source_id = command.source_id;
                    session.total_known = source_item_count <= 20000;
                    session.source_item_count = source_item_count;
                    session.current_source_offset = focused_offset;
                    session.extensions = extensions;
                    if (!session.total_known)
                    {
                        const auto current = external_hosts_.enumerate_items(
                            command.source_id,
                            command.source_window,
                            focused_offset,
                            1);
                        if (current.size() != 1 || current.front().item_id != focused_item_id ||
                            _wcsicmp(
                                current.front().filesystem_path.c_str(),
                                command.current_path.c_str()) != 0)
                        {
                            return fail(L"current_not_found");
                        }
                        auto extension = std::filesystem::path(current.front().filesystem_path)
                            .extension().wstring();
                        std::ranges::transform(
                            extension,
                            extension.begin(),
                            [](wchar_t value) { return std::towlower(value); });
                        if (!extensions.contains(extension))
                        {
                            return fail(L"current_not_found");
                        }
                        session.streaming = true;
                        response.session_id = session.id;
                        response.total_count = source_item_count;
                        response.current_index = focused_offset;
                        response.total_known = false;
                        gallery_sessions_[command.window_id] = std::move(session);
                    }
                    else
                    {
                    bool found_current{};
                    constexpr std::uint32_t source_page_size = 256;
                    for (std::uint32_t offset = 0; offset < source_item_count; offset += source_page_size)
                    {
                        report_progress();
                        if (canceled())
                        {
                            return fail(L"canceled");
                        }
                        const auto page = external_hosts_.enumerate_items(
                            command.source_id,
                            command.source_window,
                            offset,
                            std::min(source_page_size, source_item_count - offset));
                        for (const auto& item : page)
                        {
                            auto extension = std::filesystem::path(item.filesystem_path)
                                .extension().wstring();
                            std::ranges::transform(
                                extension,
                                extension.begin(),
                                [](wchar_t value) { return std::towlower(value); });
                            if (!extensions.contains(extension))
                            {
                                continue;
                            }
                            if (!found_current &&
                                (item.item_id == focused_item_id ||
                                 _wcsicmp(item.filesystem_path.c_str(), command.current_path.c_str()) == 0))
                            {
                                session.current_index = static_cast<std::uint32_t>(session.items.size());
                                found_current = true;
                            }
                            session.items.push_back(GallerySessionItem{
                                item.filesystem_path,
                                0,
                                item.item_id });
                        }
                    }
                    if (!found_current || session.items.empty())
                    {
                        return fail(L"current_not_found");
                    }
                    response.session_id = session.id;
                    response.total_count = static_cast<std::uint32_t>(session.items.size());
                    response.current_index = session.current_index;
                    response.total_known = session.total_known;
                    gallery_sessions_[command.window_id] = std::move(session);
                    }
                }
                else
                {
                ComPtr<IFolderView2> folder_view;
                const auto folder_path = std::filesystem::path(command.current_path)
                    .parent_path()
                    .wstring();
                HWND view_window{};
                if (!resolve_folder_view(
                        reinterpret_cast<HWND>(command.source_window),
                        folder_path,
                        nullptr,
                        folder_view,
                        &view_window))
                {
                    return fail(L"unavailable");
                }

                std::unordered_set<std::wstring> extensions;
                for (auto extension : command.extensions)
                {
                    std::ranges::transform(
                        extension,
                        extension.begin(),
                        [](wchar_t value) { return std::towlower(value); });
                    extensions.insert(std::move(extension));
                }
                if (extensions.empty())
                {
                    return fail(L"unsupported");
                }

                ComPtr<IShellItemArray> view_items;
                const UINT flags = static_cast<UINT>(SVGIO_ALLVIEW) |
                    static_cast<UINT>(SVGIO_FLAG_VIEWORDER);
                if (FAILED(folder_view->Items(flags, IID_PPV_ARGS(&view_items))) ||
                    view_items == nullptr)
                {
                    return fail(L"unavailable");
                }
                DWORD item_count{};
                if (FAILED(view_items->GetCount(&item_count)))
                {
                    return fail(L"unavailable");
                }

                GallerySession session;
                session.id = ++next_gallery_session_id_;
                if (session.id == 0)
                {
                    session.id = ++next_gallery_session_id_;
                }
                session.source_window = command.source_window;
                session.view_window = reinterpret_cast<std::uintptr_t>(view_window);
                session.folder_path = folder_path;
                bool found_current{};
                for (DWORD view_index = 0; view_index < item_count; ++view_index)
                {
                    if ((view_index & 63U) == 0)
                    {
                        report_progress();
                        if (canceled())
                        {
                            return fail(L"canceled");
                        }
                    }
                    ComPtr<IShellItem> item;
                    if (FAILED(view_items->GetItemAt(view_index, &item)) || item == nullptr)
                    {
                        continue;
                    }
                    SFGAOF attributes{};
                    if (FAILED(item->GetAttributes(SFGAO_FILESYSTEM | SFGAO_FOLDER, &attributes)) ||
                        (attributes & SFGAO_FILESYSTEM) == 0 ||
                        (attributes & SFGAO_FOLDER) != 0)
                    {
                        continue;
                    }
                    auto path = shell_item_name(item.Get(), SIGDN_FILESYSPATH);
                    if (path.empty())
                    {
                        continue;
                    }
                    auto extension = std::filesystem::path(path).extension().wstring();
                    std::ranges::transform(
                        extension,
                        extension.begin(),
                        [](wchar_t value) { return std::towlower(value); });
                    if (!extensions.contains(extension))
                    {
                        continue;
                    }
                    if (!found_current && _wcsicmp(path.c_str(), command.current_path.c_str()) == 0)
                    {
                        session.current_index = static_cast<std::uint32_t>(session.items.size());
                        found_current = true;
                    }
                    session.items.push_back(GallerySessionItem{
                        std::move(path),
                        view_index });
                }
                if (!found_current || session.items.empty())
                {
                    return fail(L"current_not_found");
                }

                response.session_id = session.id;
                response.total_count = static_cast<std::uint32_t>(session.items.size());
                response.current_index = session.current_index;
                gallery_sessions_[command.window_id] = std::move(session);
                }
            }

            const auto session = gallery_sessions_.find(command.window_id);
            if (session == gallery_sessions_.end() ||
                session->second.id != response.session_id)
            {
                return fail(L"stale");
            }
            auto& active_session = session->second;
            response.total_count = active_session.streaming
                ? active_session.source_item_count
                : static_cast<std::uint32_t>(active_session.items.size());
            response.current_index = active_session.streaming
                ? active_session.current_source_offset
                : active_session.current_index;
            response.total_known = active_session.total_known;

            if (active_session.streaming)
            {
                const auto matching_item = [&](std::uint32_t offset)
                    -> std::optional<ExternalHostItem> {
                    const auto items = external_hosts_.enumerate_items(
                        active_session.source_id,
                        active_session.source_window,
                        offset,
                        1);
                    if (items.size() != 1)
                    {
                        return std::nullopt;
                    }
                    auto extension = std::filesystem::path(items.front().filesystem_path)
                        .extension().wstring();
                    std::ranges::transform(
                        extension,
                        extension.begin(),
                        [](wchar_t value) { return std::towlower(value); });
                    return active_session.extensions.contains(extension)
                        ? std::optional<ExternalHostItem>(items.front())
                        : std::nullopt;
                };
                const auto find_next = [&](std::uint32_t start, int direction, bool loop)
                    -> std::optional<std::pair<std::uint32_t, ExternalHostItem>> {
                    auto offset = start;
                    for (std::uint32_t scanned = 0;
                         scanned + 1 < active_session.source_item_count;
                         ++scanned)
                    {
                        if ((scanned & 31U) == 0)
                        {
                            report_progress();
                            if (canceled())
                            {
                                return std::nullopt;
                            }
                        }
                        if (direction > 0)
                        {
                            if (offset + 1 >= active_session.source_item_count)
                            {
                                if (!loop)
                                {
                                    return std::nullopt;
                                }
                                offset = 0;
                            }
                            else
                            {
                                ++offset;
                            }
                        }
                        else if (offset == 0)
                        {
                            if (!loop)
                            {
                                return std::nullopt;
                            }
                            offset = active_session.source_item_count - 1;
                        }
                        else
                        {
                            --offset;
                        }
                        if (auto item = matching_item(offset))
                        {
                            return std::pair{ offset, std::move(*item) };
                        }
                    }
                    return std::nullopt;
                };

                auto selected_offset = active_session.current_source_offset;
                auto selected = matching_item(selected_offset);
                if (!selected)
                {
                    gallery_sessions_.erase(session);
                    return fail(L"stale");
                }
                if (command.operation == GalleryOperation::navigate)
                {
                    const int direction = command.navigation_steps < 0 ? -1 : 1;
                    for (unsigned remaining = static_cast<unsigned>(
                             std::abs(command.navigation_steps));
                         remaining > 0;
                         --remaining)
                    {
                        const auto next = find_next(selected_offset, direction, command.loop);
                        if (!next)
                        {
                            break;
                        }
                        selected_offset = next->first;
                        selected = next->second;
                    }
                    if (!external_hosts_.focus_item(
                            active_session.source_id,
                            active_session.source_window,
                            selected->item_id,
                            selected->filesystem_path))
                    {
                        gallery_sessions_.erase(session);
                        return fail(L"stale");
                    }
                    active_session.current_source_offset = selected_offset;
                    synchronized_source_window_ = active_session.source_window;
                    synchronized_path_ = selected->filesystem_path;
                    synchronized_selection_expires_ = GetTickCount64() + 2000;
                }
                else if (command.operation != GalleryOperation::open &&
                         command.operation != GalleryOperation::page)
                {
                    return fail(L"stale");
                }

                std::vector<std::pair<std::uint32_t, ExternalHostItem>> visible;
                if (const auto previous = find_next(selected_offset, -1, command.loop))
                {
                    visible.push_back(*previous);
                }
                visible.emplace_back(selected_offset, *selected);
                if (const auto next = find_next(selected_offset, 1, command.loop))
                {
                    visible.push_back(*next);
                }
                for (const auto& [offset, item] : visible)
                {
                    response.item_indices.push_back(offset);
                    auto descriptor = describe_path(item.filesystem_path);
                    if (descriptor.filesystem_path.empty())
                    {
                        descriptor.filesystem_path = item.filesystem_path;
                        descriptor.display_name = std::filesystem::path(item.filesystem_path)
                            .filename().wstring();
                        descriptor.is_filesystem = true;
                    }
                    response.items.push_back(std::move(descriptor));
                }
                response.current_index = selected_offset;
                response.success = true;
                return response;
            }

            if (command.operation == GalleryOperation::select)
            {
                if (command.target_index >= active_session.items.size())
                {
                    return fail(L"stale");
                }
                const auto& target = active_session.items[command.target_index];
                if (!active_session.source_id.empty())
                {
                    if (!external_hosts_.focus_item(
                            active_session.source_id,
                            active_session.source_window,
                            target.source_item_id,
                            target.path))
                    {
                        gallery_sessions_.erase(session);
                        return fail(L"stale");
                    }
                    active_session.current_index = command.target_index;
                    synchronized_source_window_ = active_session.source_window;
                    synchronized_path_ = target.path;
                    synchronized_selection_expires_ = GetTickCount64() + 2000;
                    response.current_index = command.target_index;
                    response.success = true;
                    return response;
                }

                ComPtr<IFolderView2> folder_view;
                ComPtr<IShellFolder> folder;
                PITEMID_CHILD item_id{};
                ComPtr<IShellItem> item;
                const bool resolved = resolve_folder_view(
                        reinterpret_cast<HWND>(active_session.source_window),
                        active_session.folder_path,
                        reinterpret_cast<HWND>(active_session.view_window),
                        folder_view) &&
                    SUCCEEDED(folder_view->GetFolder(IID_PPV_ARGS(&folder))) &&
                    SUCCEEDED(folder_view->Item(static_cast<int>(target.view_index), &item_id)) &&
                    item_id != nullptr &&
                    SUCCEEDED(SHCreateItemWithParent(
                        nullptr,
                        folder.Get(),
                        item_id,
                        IID_PPV_ARGS(&item)));
                CoTaskMemFree(item_id);
                if (!resolved || item == nullptr ||
                    _wcsicmp(
                        shell_item_name(item.Get(), SIGDN_FILESYSPATH).c_str(),
                        target.path.c_str()) != 0)
                {
                    gallery_sessions_.erase(session);
                    return fail(L"stale");
                }
                const DWORD select_flags = SVSI_SELECT | SVSI_DESELECTOTHERS |
                    SVSI_ENSUREVISIBLE | SVSI_FOCUSED;
                if (FAILED(folder_view->SelectItem(
                        static_cast<int>(target.view_index),
                        select_flags)))
                {
                    return fail(L"unavailable");
                }
                active_session.current_index = command.target_index;
                synchronized_source_window_ = active_session.source_window;
                synchronized_path_ = target.path;
                synchronized_selection_expires_ = GetTickCount64() + 2000;
                response.current_index = command.target_index;
                response.success = true;
                return response;
            }

            const std::uint32_t requested_count = std::clamp(command.page_count, 1U, 64U);
            const std::uint32_t requested_start = command.operation == GalleryOperation::open
                ? (response.current_index > requested_count / 2
                    ? response.current_index - requested_count / 2
                    : 0)
                : command.page_start;
            response.page_start = std::min<std::uint32_t>(requested_start, response.total_count);
            if (response.total_count > requested_count)
            {
                response.page_start = std::min(
                    response.page_start,
                    response.total_count - requested_count);
            }
            const auto page_end = std::min<std::uint32_t>(
                response.total_count,
                response.page_start + requested_count);
            response.items.reserve(page_end - response.page_start);
            for (std::uint32_t index = response.page_start; index < page_end; ++index)
            {
                if ((index & 7U) == 0)
                {
                    report_progress();
                    if (canceled())
                    {
                        return fail(L"canceled");
                    }
                }
                auto descriptor = describe_path(active_session.items[index].path);
                if (descriptor.filesystem_path.empty())
                {
                    descriptor.filesystem_path = active_session.items[index].path;
                    descriptor.display_name = std::filesystem::path(descriptor.filesystem_path)
                        .filename()
                        .wstring();
                    descriptor.is_filesystem = true;
                }
                response.items.push_back(std::move(descriptor));
            }
            response.success = true;
            return response;
        }
        catch (...)
        {
            return fail(L"unavailable");
        }
    }

    std::vector<ExternalHostStatus> ExplorerSelectionService::source_statuses(
        std::wstring_view language_tag)
    {
        return external_hosts_.statuses(language_tag);
    }

    bool ExplorerSelectionService::consume_gallery_selection_sync(
        const glance::contracts::SelectionSnapshot& snapshot)
    {
        if (synchronized_path_.empty())
        {
            return false;
        }
        if (GetTickCount64() > synchronized_selection_expires_)
        {
            synchronized_path_.clear();
            synchronized_source_window_ = 0;
            synchronized_selection_expires_ = 0;
            return false;
        }
        if (snapshot.source_window != synchronized_source_window_ ||
            snapshot.focused_index >= snapshot.items.size() ||
            !same_item(
                snapshot.items[snapshot.focused_index],
                synchronized_path_,
                {}))
        {
            return false;
        }
        synchronized_path_.clear();
        synchronized_source_window_ = 0;
        synchronized_selection_expires_ = 0;
        return true;
    }
}

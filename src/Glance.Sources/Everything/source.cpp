#include "pch.h"

#include "../../Glance.Contracts/include/glance/contracts/source_api.h"
#include "../../Glance.Extensions/Common/extension_localization.h"
#include "../../version.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <winver.h>

namespace
{
    using namespace glance::contracts::sources;

    constexpr wchar_t focus_window_class[] = L"EVERYTHING_RESULT_LIST_FOCUS";
    constexpr wchar_t result_list_class[] = L"SysListView32";
    constexpr std::size_t list_view_text_capacity = 32768;

    struct ListViewItem32
    {
        std::uint32_t mask{};
        std::int32_t item{};
        std::int32_t subitem{};
        std::uint32_t state{};
        std::uint32_t state_mask{};
        std::uint32_t text{};
        std::int32_t text_capacity{};
        std::int32_t image{};
        std::int32_t parameter{};
        std::int32_t indent{};
        std::int32_t group_id{};
        std::uint32_t column_count{};
        std::uint32_t columns{};
        std::uint32_t column_formats{};
        std::int32_t group{};
    };

    static_assert(sizeof(ListViewItem32) == 60);

    glance::extensions::ResourceStore resources;
    std::mutex state_mutex;
    bool connected{};
    bool gallery_available{};
    DWORD connected_process_id{};
    std::wstring connected_version;

    bool window_has_class(HWND window, const wchar_t* expected)
    {
        wchar_t class_name[128]{};
        return window != nullptr &&
            GetClassNameW(window, class_name, static_cast<int>(std::size(class_name))) > 0 &&
            _wcsicmp(class_name, expected) == 0;
    }

    HWND find_descendant_by_class(HWND root, const wchar_t* class_name)
    {
        struct Search
        {
            const wchar_t* class_name{};
            HWND result{};
        } search{ class_name };
        EnumChildWindows(root, [](HWND window, LPARAM parameter) -> BOOL {
            auto& current = *reinterpret_cast<Search*>(parameter);
            if (window_has_class(window, current.class_name))
            {
                current.result = window;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));
        return search.result;
    }

    bool text_input_active(const SourceHostContext& context)
    {
        const auto focused = reinterpret_cast<HWND>(context.focused_window);
        const auto root = reinterpret_cast<HWND>(context.root_window);
        for (HWND current = focused; current != nullptr; current = GetParent(current))
        {
            wchar_t class_name[128]{};
            if (GetClassNameW(current, class_name, static_cast<int>(std::size(class_name))) > 0 &&
                (_wcsicmp(class_name, L"Edit") == 0 ||
                 _wcsnicmp(class_name, L"RichEdit", 8) == 0))
            {
                return true;
            }
            if (current == root)
            {
                break;
            }
        }
        return context.caret_window != 0;
    }

    class RemoteListView final
    {
    public:
        ~RemoteListView()
        {
            reset();
        }

        [[nodiscard]] int item_count(HWND list, DWORD process_id)
        {
            if (!ensure_process(process_id))
            {
                return -1;
            }
            DWORD_PTR result{};
            return SendMessageTimeoutW(
                       list, LVM_GETITEMCOUNT, 0, 0,
                       SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &result) == 0
                ? -1
                : static_cast<int>(result);
        }

        [[nodiscard]] int focused_item(HWND list, DWORD process_id)
        {
            if (!ensure_process(process_id))
            {
                return -1;
            }
            DWORD_PTR result{};
            return SendMessageTimeoutW(
                       list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_FOCUSED,
                       SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &result) == 0
                ? -1
                : static_cast<int>(static_cast<std::intptr_t>(result));
        }

        [[nodiscard]] std::wstring item_path(HWND list, int item, DWORD process_id)
        {
            auto name = item_text(list, item, 0, process_id);
            auto directory = item_text(list, item, 1, process_id);
            if (name.empty() || directory.empty())
            {
                return {};
            }
            if (directory.back() != L'\\' && directory.back() != L'/')
            {
                directory.push_back(L'\\');
            }
            directory += name;
            return directory;
        }

        [[nodiscard]] bool focus_item(
            HWND list,
            int item,
            DWORD process_id,
            std::wstring_view expected_path)
        {
            if (!ensure_process(process_id) ||
                _wcsicmp(item_path(list, item, process_id).c_str(), expected_path.data()) != 0)
            {
                return false;
            }
            if (!write_state_item(0, LVIS_SELECTED | LVIS_FOCUSED))
            {
                return false;
            }
            DWORD_PTR ignored{};
            if (SendMessageTimeoutW(
                    list, LVM_SETITEMSTATE, static_cast<WPARAM>(-1),
                    reinterpret_cast<LPARAM>(remote_buffer_),
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &ignored) == 0 ||
                !write_state_item(
                    LVIS_SELECTED | LVIS_FOCUSED,
                    LVIS_SELECTED | LVIS_FOCUSED) ||
                SendMessageTimeoutW(
                    list, LVM_SETITEMSTATE, static_cast<WPARAM>(item),
                    reinterpret_cast<LPARAM>(remote_buffer_),
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &ignored) == 0 ||
                SendMessageTimeoutW(
                    list, LVM_ENSUREVISIBLE, static_cast<WPARAM>(item), FALSE,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &ignored) == 0)
            {
                return false;
            }
            return true;
        }

    private:
        template <typename Item>
        bool write_item(const Item& item) const
        {
            SIZE_T written{};
            return WriteProcessMemory(
                       process_, remote_buffer_, &item, sizeof(item), &written) != FALSE &&
                written == sizeof(item);
        }

        bool write_state_item(std::uint32_t state, std::uint32_t state_mask) const
        {
            if (target_is_32_bit_)
            {
                ListViewItem32 item;
                item.state = state;
                item.state_mask = state_mask;
                return write_item(item);
            }
            LVITEMW item{};
            item.state = state;
            item.stateMask = state_mask;
            return write_item(item);
        }

        std::wstring item_text(HWND list, int item, int subitem, DWORD process_id)
        {
            if (!ensure_process(process_id))
            {
                return {};
            }
            const auto remote_text = static_cast<std::byte*>(remote_buffer_) + item_size_;
            if (target_is_32_bit_)
            {
                ListViewItem32 list_item;
                list_item.item = item;
                list_item.subitem = subitem;
                list_item.text = static_cast<std::uint32_t>(
                    reinterpret_cast<std::uintptr_t>(remote_text));
                list_item.text_capacity = static_cast<std::int32_t>(list_view_text_capacity);
                if (!write_item(list_item))
                {
                    return {};
                }
            }
            else
            {
                LVITEMW list_item{};
                list_item.iItem = item;
                list_item.iSubItem = subitem;
                list_item.pszText = reinterpret_cast<wchar_t*>(remote_text);
                list_item.cchTextMax = static_cast<int>(list_view_text_capacity);
                if (!write_item(list_item))
                {
                    return {};
                }
            }
            DWORD_PTR result{};
            if (SendMessageTimeoutW(
                    list, LVM_GETITEMTEXTW, static_cast<WPARAM>(item),
                    reinterpret_cast<LPARAM>(remote_buffer_),
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &result) == 0)
            {
                return {};
            }
            const auto copied = static_cast<std::size_t>(result);
            if (copied == 0 || copied >= list_view_text_capacity)
            {
                return {};
            }
            std::wstring text(copied, L'\0');
            SIZE_T bytes_read{};
            const SIZE_T expected = copied * sizeof(wchar_t);
            return ReadProcessMemory(
                       process_, remote_text, text.data(), expected, &bytes_read) != FALSE &&
                    bytes_read == expected
                ? text
                : std::wstring{};
        }

        bool ensure_process(DWORD process_id)
        {
            if (process_ != nullptr && remote_buffer_ != nullptr && process_id_ == process_id)
            {
                return true;
            }
            reset();
            process_ = OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_OPERATION |
                    PROCESS_VM_READ | PROCESS_VM_WRITE,
                FALSE,
                process_id);
            if (process_ == nullptr)
            {
                return false;
            }
            USHORT process_machine{};
            USHORT native_machine{};
            if (IsWow64Process2(process_, &process_machine, &native_machine))
            {
                target_is_32_bit_ = process_machine != IMAGE_FILE_MACHINE_UNKNOWN;
            }
            else
            {
                BOOL wow64{};
                if (!IsWow64Process(process_, &wow64))
                {
                    reset();
                    return false;
                }
                target_is_32_bit_ = wow64 != FALSE;
            }
            item_size_ = target_is_32_bit_ ? sizeof(ListViewItem32) : sizeof(LVITEMW);
            remote_buffer_ = VirtualAllocEx(
                process_, nullptr,
                item_size_ + list_view_text_capacity * sizeof(wchar_t),
                MEM_COMMIT | MEM_RESERVE,
                PAGE_READWRITE);
            if (remote_buffer_ == nullptr)
            {
                reset();
                return false;
            }
            process_id_ = process_id;
            return true;
        }

        void reset()
        {
            if (remote_buffer_ != nullptr && process_ != nullptr)
            {
                VirtualFreeEx(process_, remote_buffer_, 0, MEM_RELEASE);
            }
            remote_buffer_ = nullptr;
            if (process_ != nullptr)
            {
                CloseHandle(process_);
            }
            process_ = nullptr;
            process_id_ = 0;
            item_size_ = 0;
            target_is_32_bit_ = false;
        }

        HANDLE process_{};
        void* remote_buffer_{};
        DWORD process_id_{};
        std::size_t item_size_{};
        bool target_is_32_bit_{};
    };

    RemoteListView list_reader;

    std::wstring process_version(DWORD process_id)
    {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
        if (process == nullptr)
        {
            return {};
        }
        std::wstring path(32768, L'\0');
        DWORD length = static_cast<DWORD>(path.size());
        const bool queried = QueryFullProcessImageNameW(process, 0, path.data(), &length) != FALSE;
        CloseHandle(process);
        if (!queried)
        {
            return {};
        }
        path.resize(length);
        DWORD ignored{};
        const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
        std::vector<std::byte> data(size);
        VS_FIXEDFILEINFO* info{};
        UINT info_size{};
        if (size == 0 || !GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
            !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &info_size) ||
            info == nullptr || info_size < sizeof(VS_FIXEDFILEINFO))
        {
            return {};
        }
        return std::to_wstring(HIWORD(info->dwFileVersionMS)) + L"." +
            std::to_wstring(LOWORD(info->dwFileVersionMS)) + L"." +
            std::to_wstring(HIWORD(info->dwFileVersionLS)) + L"." +
            std::to_wstring(LOWORD(info->dwFileVersionLS));
    }

    std::wstring localized(std::wstring_view key, const wchar_t* language_tag)
    {
        wchar_t value[status_detail_capacity]{};
        return resources.copy(key, language_tag, value, std::size(value))
            ? value
            : std::wstring(key);
    }

    std::wstring format_version(std::wstring text, std::wstring_view version)
    {
        const auto marker = text.find(L"{0}");
        if (marker != std::wstring::npos)
        {
            text.replace(marker, 3, version.empty() ? L"1.x" : version);
        }
        return text;
    }

    HWND valid_result_list(const SourceHostContext& context)
    {
        const auto root = reinterpret_cast<HWND>(context.root_window);
        return IsWindow(root) ? find_descendant_by_class(root, result_list_class) : nullptr;
    }

    BOOL WINAPI initialize(SourceRegistration* registration) noexcept
    {
        if (registration == nullptr || registration->size < sizeof(SourceRegistration) ||
            !resources.initialize())
        {
            return FALSE;
        }
        SourceRegistration result;
        wcscpy_s(result.source_id, L"everything");
        wcscpy_s(result.target_app_version, GLANCE_VERSION_WSTRING);
        result.capability_mask = static_cast<std::uint64_t>(Capability::selection) |
            static_cast<std::uint64_t>(Capability::item_list) |
            static_cast<std::uint64_t>(Capability::focus_change);
        *registration = result;
        return TRUE;
    }

    BOOL WINAPI query_selection(
        const SourceHostContext* context,
        SourceSelectionResult* result) noexcept
    {
        if (context == nullptr || context->size < sizeof(SourceHostContext) ||
            result == nullptr || result->size < sizeof(SourceSelectionResult))
        {
            return FALSE;
        }
        SourceSelectionResult selection;
        selection.capability_mask = static_cast<std::uint64_t>(Capability::selection);
        const auto root = reinterpret_cast<HWND>(context->root_window);
        const auto focused = reinterpret_cast<HWND>(context->focused_window);
        if (text_input_active(*context))
        {
            selection.text_input_active = TRUE;
            *result = selection;
            return TRUE;
        }
        if (context->gui_thread_flags != 0 || focused == nullptr ||
            GetAncestor(focused, GA_ROOT) != root)
        {
            *result = selection;
            return TRUE;
        }

        std::wstring path;
        const HWND focus_path = find_descendant_by_class(root, focus_window_class);
        if (focus_path != nullptr)
        {
            const int length = GetWindowTextLengthW(focus_path);
            if (length > 0 && length < static_cast<int>(path_capacity))
            {
                path.resize(static_cast<std::size_t>(length) + 1);
                const int copied = GetWindowTextW(
                    focus_path, path.data(), static_cast<int>(path.size()));
                path.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0);
                const auto line_end = path.find_first_of(L"\r\n");
                if (line_end != std::wstring::npos)
                {
                    path.resize(line_end);
                }
            }
        }

        const HWND list = valid_result_list(*context);
        if (list != nullptr)
        {
            selection.capability_mask |= static_cast<std::uint64_t>(Capability::item_list) |
                static_cast<std::uint64_t>(Capability::focus_change);
        }
        if (path.empty() && list != nullptr && focused == list)
        {
            const int item = list_reader.focused_item(list, context->process_id);
            if (item >= 0)
            {
                path = list_reader.item_path(list, item, context->process_id);
            }
        }
        if (!path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            selection.accepts_hotkey = TRUE;
            wcscpy_s(selection.filesystem_path, path.c_str());
        }
        {
            std::scoped_lock lock(state_mutex);
            connected = true;
            gallery_available = list != nullptr;
            if (connected_process_id != context->process_id)
            {
                connected_process_id = context->process_id;
                connected_version = process_version(context->process_id);
            }
        }
        *result = selection;
        return TRUE;
    }

    BOOL WINAPI query_status(
        const wchar_t* language_tag,
        SourceStatusResult* result) noexcept
    {
        if (result == nullptr || result->size < sizeof(SourceStatusResult))
        {
            return FALSE;
        }
        SourceStatusResult status;
        status.severity = HealthSeverity::healthy;
        const auto name = localized(L"Status.DisplayName", language_tag);
        wcscpy_s(status.display_name, name.c_str());
        std::scoped_lock lock(state_mutex);
        status.capability_mask = static_cast<std::uint64_t>(Capability::selection);
        std::wstring detail;
        if (!connected)
        {
            detail = localized(L"Status.Ready", language_tag);
        }
        else if (gallery_available)
        {
            status.capability_mask |= static_cast<std::uint64_t>(Capability::item_list) |
                static_cast<std::uint64_t>(Capability::focus_change);
            detail = format_version(localized(L"Status.Connected", language_tag), connected_version);
        }
        else
        {
            status.severity = HealthSeverity::warning;
            detail = format_version(localized(L"Status.Limited", language_tag), connected_version);
        }
        wcscpy_s(status.detail, detail.c_str());
        *result = status;
        return TRUE;
    }

    BOOL WINAPI query_item_count(
        const SourceHostContext* context,
        std::uint32_t* item_count,
        std::uint32_t* focused_offset,
        std::uint64_t* focused_item_id) noexcept
    {
        if (context == nullptr || item_count == nullptr || focused_offset == nullptr ||
            focused_item_id == nullptr)
        {
            return FALSE;
        }
        const HWND list = valid_result_list(*context);
        if (list == nullptr)
        {
            return FALSE;
        }
        const int count = list_reader.item_count(list, context->process_id);
        const int focused = list_reader.focused_item(list, context->process_id);
        if (count < 0 || focused < 0)
        {
            return FALSE;
        }
        *item_count = static_cast<std::uint32_t>(count);
        *focused_offset = static_cast<std::uint32_t>(focused);
        *focused_item_id = static_cast<std::uint64_t>(focused);
        return TRUE;
    }

    BOOL WINAPI enumerate_items(
        const SourceHostContext* context,
        std::uint32_t offset,
        std::uint32_t limit,
        const SourceItemSink* sink) noexcept
    {
        if (context == nullptr || sink == nullptr || sink->append == nullptr || limit == 0)
        {
            return FALSE;
        }
        const HWND list = valid_result_list(*context);
        if (list == nullptr)
        {
            return FALSE;
        }
        const int count = list_reader.item_count(list, context->process_id);
        if (count < 0 || offset > static_cast<std::uint32_t>(count))
        {
            return FALSE;
        }
        const auto end = std::min<std::uint32_t>(
            static_cast<std::uint32_t>(count), offset + limit);
        for (std::uint32_t index = offset; index < end; ++index)
        {
            const auto path = list_reader.item_path(
                list, static_cast<int>(index), context->process_id);
            if (path.empty())
            {
                continue;
            }
            const SourceItem item{ index, path.c_str() };
            if (!sink->append(sink->context, &item))
            {
                return FALSE;
            }
        }
        return TRUE;
    }

    BOOL WINAPI focus_item(
        const SourceHostContext* context,
        std::uint64_t item_id,
        const wchar_t* expected_path) noexcept
    {
        if (context == nullptr || expected_path == nullptr || item_id > INT_MAX)
        {
            return FALSE;
        }
        const HWND list = valid_result_list(*context);
        return list != nullptr && list_reader.focus_item(
            list, static_cast<int>(item_id), context->process_id, expected_path)
            ? TRUE
            : FALSE;
    }

    const ItemListApi item_list_api{
        .query_count = query_item_count,
        .enumerate = enumerate_items };
    const FocusChangeApi focus_change_api{
        .focus = focus_item };

    BOOL WINAPI query_interface(
        const GUID* interface_id,
        std::uint32_t minimum_version,
        void** interface_pointer) noexcept
    {
        if (interface_id == nullptr || interface_pointer == nullptr)
        {
            return FALSE;
        }
        *interface_pointer = nullptr;
        if (IsEqualGUID(*interface_id, item_list_api_id) &&
            minimum_version <= item_list_api_version)
        {
            *interface_pointer = const_cast<ItemListApi*>(&item_list_api);
            return TRUE;
        }
        if (IsEqualGUID(*interface_id, focus_change_api_id) &&
            minimum_version <= focus_change_api_version)
        {
            *interface_pointer = const_cast<FocusChangeApi*>(&focus_change_api);
            return TRUE;
        }
        return FALSE;
    }

    void WINAPI shutdown() noexcept
    {
        resources.shutdown();
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI GlanceSourceGetApi(
    std::uint32_t host_abi,
    glance::contracts::sources::SourceApi* api) noexcept
{
    using namespace glance::contracts::sources;
    if (host_abi != abi_version || api == nullptr || api->size < sizeof(SourceApi))
    {
        return FALSE;
    }
    SourceApi result;
    result.initialize = initialize;
    result.query_selection = query_selection;
    result.query_status = query_status;
    result.query_interface = query_interface;
    result.shutdown = shutdown;
    *api = result;
    return TRUE;
}

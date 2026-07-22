#include "external_host_provider.h"
#include "glance/contracts/diagnostics.h"

#include <commctrl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace
{
    constexpr std::array everything_process_names{
        std::wstring_view{ L"everything.exe" },
        std::wstring_view{ L"everything32.exe" },
        std::wstring_view{ L"everything64.exe" },
    };
    constexpr std::wstring_view everything_window_class = L"EVERYTHING";
    constexpr wchar_t everything_focus_window_class[] = L"EVERYTHING_RESULT_LIST_FOCUS";
    constexpr wchar_t everything_result_list_class[] = L"SysListView32";
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

    bool class_starts_with(std::wstring_view value, std::wstring_view prefix)
    {
        return value.size() >= prefix.size() &&
            _wcsnicmp(value.data(), prefix.data(), prefix.size()) == 0;
    }

    bool strings_equal(std::wstring_view left, std::wstring_view right)
    {
        return left.size() == right.size() &&
            _wcsnicmp(left.data(), right.data(), left.size()) == 0;
    }

    bool window_has_class(HWND window, const wchar_t* expected)
    {
        wchar_t class_name[128]{};
        return GetClassNameW(window, class_name, static_cast<int>(std::size(class_name))) > 0 &&
            _wcsicmp(class_name, expected) == 0;
    }

    HWND find_descendant_by_class(HWND root, const wchar_t* class_name)
    {
        struct SearchContext
        {
            const wchar_t* class_name{};
            HWND result{};
        } context{ class_name };

        EnumChildWindows(root, [](HWND window, LPARAM parameter) -> BOOL {
            auto& search = *reinterpret_cast<SearchContext*>(parameter);
            if (window_has_class(window, search.class_name))
            {
                search.result = window;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&context));
        return context.result;
    }

    bool text_input_active(const GUITHREADINFO& thread_info)
    {
        const HWND focused = thread_info.hwndFocus;
        const HWND root = focused == nullptr ? nullptr : GetAncestor(focused, GA_ROOT);
        for (HWND current = focused; current != nullptr; current = GetParent(current))
        {
            wchar_t class_name[128]{};
            if (GetClassNameW(current, class_name, static_cast<int>(std::size(class_name))) > 0 &&
                (_wcsicmp(class_name, L"Edit") == 0 || _wcsnicmp(class_name, L"RichEdit", 8) == 0))
            {
                return true;
            }
            if (current == root)
            {
                break;
            }
        }
        return thread_info.hwndCaret != nullptr;
    }

    class RemoteListViewReader final
    {
    public:
        ~RemoteListViewReader()
        {
            reset();
        }

        RemoteListViewReader(const RemoteListViewReader&) = delete;
        RemoteListViewReader& operator=(const RemoteListViewReader&) = delete;

        RemoteListViewReader() = default;

        [[nodiscard]] int focused_item(HWND list, DWORD process_id)
        {
            if (!ensure_process(process_id))
            {
                return -1;
            }

            DWORD_PTR result{};
            if (SendMessageTimeoutW(
                    list,
                    LVM_GETNEXTITEM,
                    static_cast<WPARAM>(-1),
                    LVNI_FOCUSED,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK,
                    50,
                    &result) == 0)
            {
                return -1;
            }
            return static_cast<int>(static_cast<std::intptr_t>(result));
        }

        [[nodiscard]] std::wstring item_text(HWND list, int item, int subitem, DWORD process_id)
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
                    list,
                    LVM_GETITEMTEXTW,
                    static_cast<WPARAM>(item),
                    reinterpret_cast<LPARAM>(remote_buffer_),
                    SMTO_ABORTIFHUNG | SMTO_BLOCK,
                    50,
                    &result) == 0)
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
            const SIZE_T expected_bytes = copied * sizeof(wchar_t);
            if (!ReadProcessMemory(
                    process_,
                    remote_text,
                    text.data(),
                    expected_bytes,
                    &bytes_read) ||
                bytes_read != expected_bytes)
            {
                return {};
            }
            return text;
        }

    private:
        template <typename Item>
        [[nodiscard]] bool write_item(const Item& item) const
        {
            SIZE_T bytes_written{};
            return WriteProcessMemory(
                       process_,
                       remote_buffer_,
                       &item,
                       sizeof(item),
                       &bytes_written) != FALSE &&
                bytes_written == sizeof(item);
        }

        [[nodiscard]] bool ensure_process(DWORD process_id)
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
            if (IsWow64Process2(process_, &process_machine, &native_machine) != FALSE)
            {
                target_is_32_bit_ = process_machine != IMAGE_FILE_MACHINE_UNKNOWN;
            }
            else
            {
                BOOL wow64{};
                if (IsWow64Process(process_, &wow64) == FALSE)
                {
                    reset();
                    return false;
                }
                target_is_32_bit_ = wow64 != FALSE;
            }

            item_size_ = target_is_32_bit_ ? sizeof(ListViewItem32) : sizeof(LVITEMW);
            const SIZE_T allocation_size =
                item_size_ + list_view_text_capacity * sizeof(wchar_t);
            remote_buffer_ = VirtualAllocEx(
                process_,
                nullptr,
                allocation_size,
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

    class ExternalHostProvider
    {
    public:
        virtual ~ExternalHostProvider() = default;
        [[nodiscard]] virtual glance::core::ExternalHostSelection query(
            const glance::core::ExternalHostContext& context) = 0;
    };

    class EverythingHostProvider final : public ExternalHostProvider
    {
    public:
        [[nodiscard]] glance::core::ExternalHostSelection query(
            const glance::core::ExternalHostContext& context) override
        {
            glance::core::ExternalHostSelection selection;
            selection.host_kind = glance::contracts::HostKind::everything;

            if (text_input_active(context.thread_info) ||
                context.thread_info.flags != 0 ||
                context.thread_info.hwndFocus == nullptr ||
                GetAncestor(context.thread_info.hwndFocus, GA_ROOT) != context.root_window)
            {
                return selection;
            }

            const HWND focus_path_window = find_descendant_by_class(
                context.root_window,
                everything_focus_window_class);
            if (focus_path_window != nullptr)
            {
                const int expected_length = GetWindowTextLengthW(focus_path_window);
                if (expected_length <= 0 || expected_length > 32767)
                {
                    return selection;
                }

                std::wstring path(static_cast<std::size_t>(expected_length) + 1, L'\0');
                const int actual_length = GetWindowTextW(
                    focus_path_window,
                    path.data(),
                    static_cast<int>(path.size()));
                if (actual_length <= 0)
                {
                    return selection;
                }
                path.resize(static_cast<std::size_t>(actual_length));
                const auto line_end = path.find_first_of(L"\r\n");
                if (line_end != std::wstring::npos)
                {
                    path.resize(line_end);
                }
                if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
                {
                    return selection;
                }

                selection.accepts_hotkey = true;
                selection.filesystem_path = std::move(path);
                return selection;
            }

            const HWND result_list = find_descendant_by_class(
                context.root_window,
                everything_result_list_class);
            if (result_list == nullptr || context.thread_info.hwndFocus != result_list)
            {
                return selection;
            }

            const int focused_item = list_view_reader_.focused_item(result_list, context.process_id);
            if (focused_item < 0)
            {
                return selection;
            }

            auto name = list_view_reader_.item_text(
                result_list,
                focused_item,
                0,
                context.process_id);
            auto directory = list_view_reader_.item_text(
                result_list,
                focused_item,
                1,
                context.process_id);
            if (name.empty() || directory.empty())
            {
                return selection;
            }
            if (directory.back() != L'\\' && directory.back() != L'/')
            {
                directory.push_back(L'\\');
            }
            directory += name;
            if (GetFileAttributesW(directory.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                return selection;
            }

            selection.accepts_hotkey = true;
            selection.filesystem_path = std::move(directory);
            return selection;
        }

    private:
        RemoteListViewReader list_view_reader_;
    };

    using Matcher = bool (*)(const glance::core::ExternalHostContext&);
    using Factory = std::unique_ptr<ExternalHostProvider> (*)();

    struct ProviderRegistration
    {
        std::wstring_view name;
        Matcher matches{};
        Factory create{};
    };

    bool matches_everything(const glance::core::ExternalHostContext& context)
    {
        return std::ranges::any_of(everything_process_names, [&](std::wstring_view process_name) {
            return strings_equal(context.process_name, process_name);
        }) && class_starts_with(context.window_class, everything_window_class);
    }

    std::unique_ptr<ExternalHostProvider> create_everything_provider()
    {
        return std::make_unique<EverythingHostProvider>();
    }

    constexpr std::array registrations{
        ProviderRegistration{
            L"Everything",
            matches_everything,
            create_everything_provider,
        },
    };
}

namespace glance::core
{
    struct ExternalHostProviderRegistry::Impl
    {
        std::unique_ptr<ExternalHostProvider> active_provider;
        std::size_t active_registration{ registrations.size() };
        DWORD active_process_id{};
    };

    ExternalHostProviderRegistry::ExternalHostProviderRegistry() : impl_(std::make_unique<Impl>()) {}
    ExternalHostProviderRegistry::~ExternalHostProviderRegistry() = default;

    std::optional<ExternalHostSelection> ExternalHostProviderRegistry::query(
        const ExternalHostContext& context)
    {
        std::size_t matching_registration = registrations.size();
        for (std::size_t index = 0; index < registrations.size(); ++index)
        {
            if (registrations[index].matches(context))
            {
                matching_registration = index;
                break;
            }
        }

        if (matching_registration == registrations.size())
        {
            impl_->active_provider.reset();
            impl_->active_registration = registrations.size();
            impl_->active_process_id = 0;
            return std::nullopt;
        }

        if (impl_->active_provider == nullptr ||
            impl_->active_registration != matching_registration ||
            impl_->active_process_id != context.process_id)
        {
            impl_->active_provider = registrations[matching_registration].create();
            impl_->active_registration = matching_registration;
            impl_->active_process_id = context.process_id;
            glance::contracts::log_event(
                L"Activated external host provider: " +
                std::wstring(registrations[matching_registration].name) + L".");
        }
        return impl_->active_provider->query(context);
    }
}

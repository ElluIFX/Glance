#include "core_application.h"
#include "explorer_selection.h"
#include "glance/contracts/diagnostics.h"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <tlhelp32.h>
#include <wtsapi32.h>

namespace
{
    std::optional<std::uint64_t> parse_u64(const winrt::hstring& value)
    {
        try
        {
            std::size_t consumed{};
            const auto parsed = std::stoull(value.c_str(), &consumed);
            return consumed == value.size()
                ? std::optional<std::uint64_t>(parsed)
                : std::nullopt;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::wstring_view gallery_operation_name(glance::core::GalleryOperation operation) noexcept
    {
        using glance::core::GalleryOperation;
        switch (operation)
        {
        case GalleryOperation::open:
            return L"open";
        case GalleryOperation::page:
            return L"page";
        case GalleryOperation::select:
            return L"select";
        case GalleryOperation::navigate:
            return L"navigate";
        case GalleryOperation::close:
            return L"close";
        }
        return L"";
    }

    std::optional<glance::core::GalleryCommand> parse_gallery_command(std::string_view payload)
    {
        using namespace winrt::Windows::Data::Json;
        try
        {
            const auto root = JsonObject::Parse(winrt::to_hstring(payload));
            glance::core::GalleryCommand command;
            const std::wstring operation = root.GetNamedString(L"operation").c_str();
            if (operation == L"open")
            {
                command.operation = glance::core::GalleryOperation::open;
            }
            else if (operation == L"page")
            {
                command.operation = glance::core::GalleryOperation::page;
            }
            else if (operation == L"select")
            {
                command.operation = glance::core::GalleryOperation::select;
            }
            else if (operation == L"navigate")
            {
                command.operation = glance::core::GalleryOperation::navigate;
            }
            else if (operation == L"close")
            {
                command.operation = glance::core::GalleryOperation::close;
            }
            else
            {
                return std::nullopt;
            }

            const auto window_id = parse_u64(root.GetNamedString(L"windowId"));
            const auto request_id = parse_u64(root.GetNamedString(L"requestId"));
            const auto session_id = parse_u64(root.GetNamedString(L"sessionId", L"0"));
            if (!window_id || !request_id || !session_id)
            {
                return std::nullopt;
            }
            command.window_id = *window_id;
            command.request_id = *request_id;
            command.session_id = *session_id;
            command.source_window = static_cast<std::uintptr_t>(
                parse_u64(root.GetNamedString(L"sourceWindow", L"0")).value_or(0));
            command.source_id = root.GetNamedString(L"sourceId", L"").c_str();
            command.current_path = root.GetNamedString(L"currentPath", L"").c_str();
            command.page_start = static_cast<std::uint32_t>(
                std::max(0.0, root.GetNamedNumber(L"pageStart", 0)));
            command.page_count = static_cast<std::uint32_t>(
                std::max(0.0, root.GetNamedNumber(L"pageCount", 64)));
            command.target_index = static_cast<std::uint32_t>(
                std::max(0.0, root.GetNamedNumber(L"targetIndex", 0)));
            command.navigation_steps = static_cast<int>(
                root.GetNamedNumber(L"navigationSteps", 0));
            command.loop = root.GetNamedBoolean(L"loop", false);
            if (command.operation == glance::core::GalleryOperation::open)
            {
                const auto extensions = root.GetNamedArray(L"extensions");
                command.extensions.reserve(extensions.Size());
                for (std::uint32_t index = 0; index < extensions.Size(); ++index)
                {
                    command.extensions.emplace_back(extensions.GetStringAt(index).c_str());
                }
            }
            return command;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::string make_gallery_response_payload(const glance::core::GalleryResponse& response)
    {
        using namespace winrt::Windows::Data::Json;
        JsonObject root;
        root.SetNamedValue(
            L"operation",
            JsonValue::CreateStringValue(gallery_operation_name(response.operation)));
        root.SetNamedValue(L"windowId", JsonValue::CreateStringValue(std::to_wstring(response.window_id)));
        root.SetNamedValue(L"requestId", JsonValue::CreateStringValue(std::to_wstring(response.request_id)));
        root.SetNamedValue(L"sessionId", JsonValue::CreateStringValue(std::to_wstring(response.session_id)));
        root.SetNamedValue(L"success", JsonValue::CreateBooleanValue(response.success));
        root.SetNamedValue(L"error", JsonValue::CreateStringValue(response.error));
        root.SetNamedValue(L"totalCount", JsonValue::CreateNumberValue(response.total_count));
        root.SetNamedValue(L"currentIndex", JsonValue::CreateNumberValue(response.current_index));
        root.SetNamedValue(L"pageStart", JsonValue::CreateNumberValue(response.page_start));
        root.SetNamedValue(L"totalKnown", JsonValue::CreateBooleanValue(response.total_known));
        JsonArray items;
        for (std::size_t index = 0; index < response.items.size(); ++index)
        {
            const auto& item = response.items[index];
            JsonObject file;
            file.SetNamedValue(
                L"index",
                JsonValue::CreateNumberValue(
                    index < response.item_indices.size()
                        ? response.item_indices[index]
                        : response.page_start + static_cast<std::uint32_t>(index)));
            file.SetNamedValue(L"displayName", JsonValue::CreateStringValue(item.display_name));
            file.SetNamedValue(L"path", JsonValue::CreateStringValue(item.filesystem_path));
            file.SetNamedValue(L"parsingName", JsonValue::CreateStringValue(item.shell_parsing_name));
            file.SetNamedValue(L"size", JsonValue::CreateStringValue(std::to_wstring(item.size)));
            file.SetNamedValue(L"creationTime", JsonValue::CreateStringValue(std::to_wstring(item.creation_time)));
            file.SetNamedValue(L"lastWriteTime", JsonValue::CreateStringValue(std::to_wstring(item.last_write_time)));
            file.SetNamedValue(L"attributes", JsonValue::CreateNumberValue(item.attributes));
            file.SetNamedValue(L"isFilesystem", JsonValue::CreateBooleanValue(item.is_filesystem));
            file.SetNamedValue(L"isCloudPlaceholder", JsonValue::CreateBooleanValue(item.is_cloud_placeholder));
            items.Append(file);
        }
        root.SetNamedValue(L"items", items);
        return winrt::to_string(root.Stringify());
    }

    std::string make_source_status_payload(
        const std::vector<glance::core::ExternalHostStatus>& statuses)
    {
        using namespace winrt::Windows::Data::Json;
        JsonArray entries;
        for (const auto& status : statuses)
        {
            JsonObject entry;
            entry.SetNamedValue(L"id", JsonValue::CreateStringValue(status.source_id));
            entry.SetNamedValue(L"name", JsonValue::CreateStringValue(status.display_name));
            entry.SetNamedValue(L"detail", JsonValue::CreateStringValue(status.detail));
            entry.SetNamedValue(L"severity", JsonValue::CreateNumberValue(status.severity));
            entry.SetNamedValue(L"code", JsonValue::CreateNumberValue(status.code));
            entry.SetNamedValue(
                L"capabilities",
                JsonValue::CreateStringValue(std::to_wstring(status.capabilities)));
            entries.Append(entry);
        }
        JsonObject root;
        root.SetNamedValue(L"sources", entries);
        return winrt::to_string(root.Stringify());
    }

    bool process_is_elevated() noexcept
    {
        HANDLE raw_token{};
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token))
        {
            return false;
        }
        glance::core::unique_handle token(raw_token);
        TOKEN_ELEVATION elevation{};
        DWORD returned_size{};
        return GetTokenInformation(
                   token.get(),
                   TokenElevation,
                   &elevation,
                   sizeof(elevation),
                   &returned_size) != FALSE &&
            elevation.TokenIsElevated != 0;
    }

    std::filesystem::path executable_directory()
    {
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

    bool paths_equal(std::wstring_view left, std::wstring_view right) noexcept
    {
        return CompareStringOrdinal(
                   left.data(),
                   static_cast<int>(left.size()),
                   right.data(),
                   static_cast<int>(right.size()),
                   TRUE) == CSTR_EQUAL;
    }

    HANDLE open_supervised_process(DWORD process_id) noexcept
    {
        HANDLE process = OpenProcess(
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
            FALSE,
            process_id);
        if (process == nullptr)
        {
            process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
        }
        return process;
    }

    DWORD find_process_by_path(const std::filesystem::path& expected_path) noexcept
    {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return 0;
        }

        DWORD current_session{};
        static_cast<void>(ProcessIdToSessionId(GetCurrentProcessId(), &current_session));
        PROCESSENTRY32W entry{ sizeof(PROCESSENTRY32W) };
        DWORD result{};
        if (Process32FirstW(snapshot, &entry))
        {
            do
            {
                DWORD session{};
                if (!ProcessIdToSessionId(entry.th32ProcessID, &session) || session != current_session)
                {
                    continue;
                }
                glance::core::unique_handle process(open_supervised_process(entry.th32ProcessID));
                if (!process)
                {
                    continue;
                }
                std::wstring path(32768, L'\0');
                DWORD length = static_cast<DWORD>(path.size());
                if (QueryFullProcessImageNameW(process.get(), 0, path.data(), &length))
                {
                    path.resize(length);
                    if (paths_equal(path, expected_path.wstring()))
                    {
                        result = entry.th32ProcessID;
                        break;
                    }
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return result;
    }

    HANDLE duplicate_primary_token(HANDLE process) noexcept
    {
        HANDLE raw_token{};
        if (!OpenProcessToken(
                process,
                TOKEN_QUERY | TOKEN_DUPLICATE,
                &raw_token))
        {
            return nullptr;
        }
        glance::core::unique_handle token(raw_token);
        HANDLE primary_token{};
        if (!DuplicateTokenEx(
                token.get(),
                TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY |
                    TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
                nullptr,
                SecurityImpersonation,
                TokenPrimary,
                &primary_token))
        {
            return nullptr;
        }
        return primary_token;
    }

    HANDLE shell_primary_token() noexcept
    {
        const HWND shell_window = GetShellWindow();
        DWORD shell_process_id{};
        if (shell_window == nullptr || GetWindowThreadProcessId(shell_window, &shell_process_id) == 0)
        {
            return nullptr;
        }
        glance::core::unique_handle shell_process(OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            shell_process_id));
        return shell_process ? duplicate_primary_token(shell_process.get()) : nullptr;
    }
}

namespace glance::core
{
    struct SelectionWorkerContext
    {
        unique_handle stop_event;
        unique_handle gallery_event;
        std::atomic<HWND> window{};
        UINT result_message{};
        UINT gallery_result_message{};
        UINT source_status_result_message{};
        std::atomic_uint64_t generation{ 1 };
        std::atomic_uint64_t progress_ms{};
        std::atomic_uint64_t completed_generation{};
        std::mutex pending_mutex;
        std::optional<glance::contracts::SelectionSnapshot> pending_selection;
        std::uint64_t pending_generation{};
        bool pending_selection_suppressed{};
        std::atomic_bool message_pending{};
        std::mutex gallery_mutex;
        std::deque<GalleryCommand> gallery_commands;
        std::deque<GalleryResponse> gallery_responses;
        std::unordered_map<std::uint64_t, std::uint64_t> latest_gallery_requests;
        std::atomic_bool gallery_result_pending{};
        std::optional<std::wstring> source_status_language;
        std::deque<std::string> source_status_responses;
        std::atomic_bool source_status_result_pending{};
    };

    CoreApplication::CoreApplication()
        : pipe_server_(
              [this](auto type, auto flags, auto payload) { handle_pipe_message(type, flags, payload); },
              [this](bool connected) { handle_connection_changed(connected); })
    {
    }

    CoreApplication::~CoreApplication()
    {
        stop_selection_worker();
        if (window_ != nullptr)
        {
            WTSUnRegisterSessionNotification(window_);
        }
        if (keyboard_hook_ != nullptr)
        {
            keyboard_hook_->stop();
            delete keyboard_hook_;
            keyboard_hook_ = nullptr;
        }
        pipe_server_.stop();
    }

    int CoreApplication::run(HINSTANCE instance, DWORD app_process_id)
    {
        single_instance_mutex_.reset(CreateMutexW(nullptr, FALSE, L"Local\\Glance.Core"));
        if (!single_instance_mutex_ || GetLastError() == ERROR_ALREADY_EXISTS)
        {
            return 0;
        }
        shutdown_event_.reset(CreateEventW(nullptr, TRUE, FALSE, L"Local\\Glance.Shutdown"));
        elevated_ = process_is_elevated();
        if (elevated_)
        {
            elevated_status_mutex_.reset(CreateMutexW(
                nullptr,
                FALSE,
                L"Local\\Glance.Core.Elevated"));
        }

        winrt::init_apartment(winrt::apartment_type::single_threaded);
        if (!create_message_window(instance))
        {
            glance::contracts::log_event(L"Failed to create the Core message window.");
            return 1;
        }

        if (app_process_id != 0)
        {
            capture_app_process(app_process_id);
        }

        RAWINPUTDEVICE keyboard{};
        keyboard.usUsagePage = 0x01;
        keyboard.usUsage = 0x06;
        keyboard.dwFlags = RIDEV_INPUTSINK;
        keyboard.hwndTarget = window_;
        if (!RegisterRawInputDevices(&keyboard, 1, sizeof(keyboard)))
        {
            glance::contracts::log_event(
                L"RegisterRawInputDevices failed with error " + std::to_wstring(GetLastError()) + L".");
        }

        static_cast<void>(pipe_server_.start());
        keyboard_hook_ = new KeyboardHookService(window_, hook_action_message, input_state_);
        if (!keyboard_hook_->start())
        {
            glance::contracts::log_event(L"Failed to start the keyboard hook.");
            return 2;
        }

        if (!start_selection_worker())
        {
            glance::contracts::log_event(L"Failed to start the selection worker.");
            return 3;
        }
        SetTimer(window_, selection_timer_id, 100, nullptr);
        SetTimer(window_, hook_refresh_timer_id, hook_refresh_interval_ms, nullptr);
        SetTimer(window_, app_watchdog_timer_id, app_watchdog_interval_ms, nullptr);
        static_cast<void>(WTSRegisterSessionNotification(window_, NOTIFY_FOR_THIS_SESSION));
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

    bool CoreApplication::create_message_window(HINSTANCE instance)
    {
        constexpr wchar_t class_name[] = L"Glance.Core.MessageWindow";
        WNDCLASSEXW window_class{ sizeof(WNDCLASSEXW) };
        window_class.hInstance = instance;
        window_class.lpfnWndProc = window_proc;
        window_class.lpszClassName = class_name;
        if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return false;
        }

        window_ = CreateWindowExW(
            WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            class_name,
            L"",
            WS_POPUP,
            0,
            0,
            0,
            0,
            nullptr,
            nullptr,
            instance,
            this);
        return window_ != nullptr;
    }

    LRESULT CALLBACK CoreApplication::window_proc(
        HWND window,
        UINT message,
        WPARAM wparam,
        LPARAM lparam) noexcept
    {
        CoreApplication* self = reinterpret_cast<CoreApplication*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            self = static_cast<CoreApplication*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self == nullptr)
        {
            return DefWindowProcW(window, message, wparam, lparam);
        }

        switch (message)
        {
        case WM_TIMER:
            if (wparam == selection_timer_id)
            {
                const auto now = GetTickCount64();
                if (self->selection_.timestamp_ms == 0 ||
                    now - self->selection_.timestamp_ms > selection_stale_after_ms)
                {
                    self->input_state_.eligible_selection.store(false, std::memory_order_release);
                }
                self->monitor_selection_worker();
                return 0;
            }
            if (wparam == hook_refresh_timer_id && self->keyboard_hook_ != nullptr)
            {
                const auto raw_count = self->raw_input_count_.load(std::memory_order_relaxed);
                const auto hook_count = self->keyboard_hook_->event_count();
                const bool stalled = raw_count != self->previous_raw_input_count_ &&
                    hook_count == self->previous_hook_event_count_;
                self->previous_raw_input_count_ = raw_count;
                self->previous_hook_event_count_ = hook_count;
                if (stalled)
                {
                    self->recover_keyboard_hook(L"Raw Input advanced while hook events stalled");
                }
                return 0;
            }
            if (wparam == app_watchdog_timer_id)
            {
                self->supervise_app();
                return 0;
            }
            break;
        case WM_POWERBROADCAST:
            if (wparam == PBT_APMRESUMEAUTOMATIC && self->keyboard_hook_ != nullptr)
            {
                self->recover_keyboard_hook(L"System resume");
            }
            return TRUE;
        case WM_WTSSESSION_CHANGE:
            if (wparam == WTS_SESSION_UNLOCK && self->keyboard_hook_ != nullptr)
            {
                self->recover_keyboard_hook(L"Session unlock");
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_INPUT:
            self->raw_input_count_.fetch_add(1, std::memory_order_relaxed);
            return 0;
        case hook_action_message:
            self->handle_hook_action(
                static_cast<HookAction>(wparam),
                static_cast<std::uint64_t>(lparam));
            return 0;
        case selection_result_message:
            self->apply_pending_selection();
            return 0;
        case gallery_command_message:
            self->apply_pending_gallery_commands();
            return 0;
        case gallery_result_message:
            self->apply_pending_gallery_responses();
            return 0;
        case source_status_result_message:
            self->apply_pending_source_status_responses();
            return 0;
        case heartbeat_message:
            if (self->selection_worker_healthy())
            {
                static_cast<void>(self->pipe_server_.send(
                    glance::contracts::MessageType::heartbeat_ack,
                    {},
                    static_cast<std::uint32_t>(wparam)));
            }
            return 0;
        case connection_changed_message:
            self->reset_app_health();
            if (wparam != 0)
            {
                self->capture_app_process(static_cast<DWORD>(lparam));
                self->app_connection_grace_until_ms_ = 0;
                if (self->keyboard_hook_ != nullptr)
                {
                    self->recover_keyboard_hook(L"UI connection restored");
                }
                self->input_state_.ui_connected.store(true, std::memory_order_release);
            }
            else
            {
                self->app_connection_grace_until_ms_ = GetTickCount64();
            }
            return 0;
        case WM_DESTROY:
            self->stop_selection_worker();
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void CoreApplication::recover_keyboard_hook(std::wstring_view reason)
    {
        if (keyboard_hook_ == nullptr)
        {
            return;
        }
        if (keyboard_hook_->refresh())
        {
            glance::contracts::log_event(L"Keyboard hook recovered: " + std::wstring(reason) + L".");
            return;
        }

        glance::contracts::log_event(
            L"Keyboard hook refresh failed; rebuilding service: " + std::wstring(reason) + L".");
        keyboard_hook_->stop();
        delete keyboard_hook_;
        keyboard_hook_ = new KeyboardHookService(window_, hook_action_message, input_state_);
        if (!keyboard_hook_->start())
        {
            glance::contracts::log_event(L"Keyboard hook service rebuild failed.");
        }
    }

    void CoreApplication::supervise_app()
    {
        if (shutting_down_.load(std::memory_order_acquire))
        {
            return;
        }
        if (shutdown_event_ && WaitForSingleObject(shutdown_event_.get(), 0) == WAIT_OBJECT_0)
        {
            shutting_down_.store(true, std::memory_order_release);
            PostMessageW(window_, WM_CLOSE, 0, 0);
            return;
        }

        const DWORD connected_process_id = pipe_server_.connected()
            ? pipe_server_.peer_process_id()
            : 0;
        if (connected_process_id != 0 && connected_process_id != app_process_id_)
        {
            capture_app_process(connected_process_id);
        }
        if (!app_process_)
        {
            const DWORD process_id = find_process_by_path(executable_directory() / L"Glance.exe");
            if (process_id != 0)
            {
                capture_app_process(process_id);
            }
        }
        if (!app_process_)
        {
            reset_app_health();
            static_cast<void>(launch_app());
            return;
        }
        if (WaitForSingleObject(app_process_.get(), 0) == WAIT_OBJECT_0)
        {
            glance::contracts::log_event(L"UI process exited unexpectedly; restarting it.");
            input_state_.ui_connected.store(false, std::memory_order_release);
            preview_state_.store(
                glance::contracts::PreviewWindowState::hidden,
                std::memory_order_release);
            input_state_.preview_active.store(false, std::memory_order_release);
            close_app_process();
            reset_app_health();
            static_cast<void>(launch_app());
            return;
        }

        const auto now = GetTickCount64();
        if (!pipe_server_.connected())
        {
            pending_heartbeat_ = 0;
            if (now < app_connection_grace_until_ms_)
            {
                return;
            }
            if (++missed_heartbeats_ >= glance::contracts::process_watchdog_failure_limit)
            {
                terminate_unresponsive_app();
            }
            return;
        }

        if (pending_heartbeat_ != 0)
        {
            const auto acknowledged = last_heartbeat_ack_.load(std::memory_order_acquire);
            if (glance::contracts::heartbeat_acknowledged(pending_heartbeat_, acknowledged))
            {
                missed_heartbeats_ = 0;
            }
            else if (++missed_heartbeats_ >= glance::contracts::process_watchdog_failure_limit)
            {
                terminate_unresponsive_app();
                return;
            }
        }

        const auto sequence = ++heartbeat_sequence_;
        if (pipe_server_.send(glance::contracts::MessageType::heartbeat, {}, sequence))
        {
            pending_heartbeat_ = sequence;
        }
    }

    void CoreApplication::capture_app_process(DWORD process_id)
    {
        if (process_id == 0)
        {
            return;
        }
        if (process_id == app_process_id_ && app_process_ &&
            WaitForSingleObject(app_process_.get(), 0) == WAIT_TIMEOUT)
        {
            if (!app_token_)
            {
                app_token_.reset(duplicate_primary_token(app_process_.get()));
            }
            return;
        }

        unique_handle process(open_supervised_process(process_id));
        if (!process)
        {
            glance::contracts::log_event(
                L"Could not open the UI process " + std::to_wstring(process_id) + L" for supervision.");
            return;
        }
        std::wstring path(32768, L'\0');
        DWORD length = static_cast<DWORD>(path.size());
        const auto expected_path = (executable_directory() / L"Glance.exe").wstring();
        if (!QueryFullProcessImageNameW(process.get(), 0, path.data(), &length))
        {
            return;
        }
        path.resize(length);
        if (!paths_equal(path, expected_path))
        {
            glance::contracts::log_event(L"Rejected a UI supervision target with an unexpected path.");
            return;
        }

        unique_handle token(duplicate_primary_token(process.get()));
        close_app_process();
        app_process_ = std::move(process);
        app_process_id_ = process_id;
        if (token)
        {
            app_token_ = std::move(token);
        }
        app_connection_grace_until_ms_ =
            GetTickCount64() + glance::contracts::process_watchdog_connect_grace_ms;
    }

    void CoreApplication::close_app_process() noexcept
    {
        app_process_.reset();
        app_process_id_ = 0;
    }

    void CoreApplication::reset_app_health() noexcept
    {
        pending_heartbeat_ = 0;
        missed_heartbeats_ = 0;
        last_heartbeat_ack_.store(0, std::memory_order_release);
    }

    void CoreApplication::terminate_unresponsive_app()
    {
        glance::contracts::log_event(
            L"UI health check failed " +
            std::to_wstring(glance::contracts::process_watchdog_failure_limit) +
            L" consecutive times; terminating it for recovery.");
        input_state_.ui_connected.store(false, std::memory_order_release);
        preview_state_.store(
            glance::contracts::PreviewWindowState::hidden,
            std::memory_order_release);
        input_state_.preview_active.store(false, std::memory_order_release);
        static_cast<void>(pipe_server_.send(glance::contracts::MessageType::terminate_unresponsive));
        if (app_process_)
        {
            static_cast<void>(TerminateProcess(app_process_.get(), ERROR_PROCESS_ABORTED));
        }
        reset_app_health();
        app_connection_grace_until_ms_ =
            GetTickCount64() + glance::contracts::process_watchdog_interval_ms;
    }

    bool CoreApplication::launch_app()
    {
        if (shutting_down_.load(std::memory_order_acquire))
        {
            return false;
        }
        unique_handle app_mutex(OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\Glance.App"));
        if (app_mutex)
        {
            return false;
        }

        const auto now = GetTickCount64();
        if (last_app_launch_attempt_ms_ != 0 && now - last_app_launch_attempt_ms_ < 2000)
        {
            return false;
        }
        last_app_launch_attempt_ms_ = now;

        const auto app_path = executable_directory() / L"Glance.exe";
        if (!std::filesystem::exists(app_path))
        {
            glance::contracts::log_event(L"Glance.exe was not found for UI recovery.");
            return false;
        }

        std::wstring command_line = L"\"" + app_path.wstring() + L"\"";
        const auto working_directory = app_path.parent_path().wstring();
        STARTUPINFOW startup{ sizeof(STARTUPINFOW) };
        PROCESS_INFORMATION process{};
        BOOL created{};
        if (elevated_)
        {
            unique_handle shell_token;
            HANDLE launch_token = app_token_.get();
            if (launch_token == nullptr)
            {
                shell_token.reset(shell_primary_token());
                launch_token = shell_token.get();
            }
            if (launch_token != nullptr)
            {
                created = CreateProcessWithTokenW(
                    launch_token,
                    LOGON_WITH_PROFILE,
                    app_path.c_str(),
                    command_line.data(),
                    0,
                    nullptr,
                    working_directory.c_str(),
                    &startup,
                    &process);
            }
        }
        else
        {
            created = CreateProcessW(
                app_path.c_str(),
                command_line.data(),
                nullptr,
                nullptr,
                FALSE,
                0,
                nullptr,
                working_directory.c_str(),
                &startup,
                &process);
        }
        if (!created)
        {
            glance::contracts::log_event(
                L"UI recovery launch failed with error " + std::to_wstring(GetLastError()) + L".");
            return false;
        }

        CloseHandle(process.hThread);
        close_app_process();
        app_process_.reset(process.hProcess);
        app_process_id_ = process.dwProcessId;
        if (!app_token_)
        {
            app_token_.reset(duplicate_primary_token(process.hProcess));
        }
        app_connection_grace_until_ms_ =
            GetTickCount64() + glance::contracts::process_watchdog_connect_grace_ms;
        reset_app_health();
        glance::contracts::log_event(L"UI process recovery launch requested.");
        return true;
    }

    bool CoreApplication::start_selection_worker()
    {
        auto context = std::make_shared<SelectionWorkerContext>();
        context->stop_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        context->gallery_event.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!context->stop_event || !context->gallery_event)
        {
            return false;
        }
        context->window.store(window_, std::memory_order_release);
        context->result_message = selection_result_message;
        context->gallery_result_message = gallery_result_message;
        context->source_status_result_message = source_status_result_message;
        context->progress_ms.store(GetTickCount64(), std::memory_order_release);
        context->completed_generation.store(1, std::memory_order_release);
        selection_worker_context_ = std::move(context);
        selection_worker_last_restart_ms_ = GetTickCount64();
        return start_selection_worker_generation();
    }

    bool CoreApplication::start_selection_worker_generation()
    {
        const auto context = selection_worker_context_;
        if (!context)
        {
            return false;
        }
        const auto generation = context->generation.load(std::memory_order_acquire);
        try
        {
            selection_worker_ = std::thread(
                &CoreApplication::run_selection_worker,
                context,
                generation);
        }
        catch (...)
        {
            return false;
        }
        return true;
    }

    void CoreApplication::run_selection_worker(
        std::shared_ptr<SelectionWorkerContext> context,
        std::uint64_t generation)
    {
        const HRESULT apartment_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(apartment_result))
        {
            glance::contracts::log_event(
                L"Selection worker COM initialization failed with HRESULT " +
                std::to_wstring(static_cast<unsigned long>(apartment_result)) + L".");
            return;
        }

        try
        {
            ExplorerSelectionService selection_service;
            while (WaitForSingleObject(context->stop_event.get(), 0) == WAIT_TIMEOUT &&
                   context->generation.load(std::memory_order_acquire) == generation)
            {
                std::optional<GalleryCommand> gallery_command;
                {
                    const std::scoped_lock lock(context->gallery_mutex);
                    if (!context->gallery_commands.empty())
                    {
                        gallery_command = std::move(context->gallery_commands.front());
                        context->gallery_commands.pop_front();
                    }
                }
                if (gallery_command)
                {
                    const auto is_latest = [context, command = *gallery_command] {
                        const std::scoped_lock lock(context->gallery_mutex);
                        const auto latest = context->latest_gallery_requests.find(command.window_id);
                        return latest != context->latest_gallery_requests.end() &&
                            latest->second == command.request_id;
                    };
                    if (is_latest())
                    {
                        auto response = selection_service.handle_gallery_command(
                            *gallery_command,
                            [context, generation, is_latest] {
                                return context->generation.load(std::memory_order_acquire) != generation ||
                                    WaitForSingleObject(context->stop_event.get(), 0) != WAIT_TIMEOUT ||
                                    !is_latest();
                            },
                            [context] {
                                context->progress_ms.store(GetTickCount64(), std::memory_order_release);
                            });
                        if (!response.success && response.error != L"canceled")
                        {
                            glance::contracts::log_event(
                                L"Gallery " +
                                std::wstring(gallery_operation_name(response.operation)) +
                                L" request failed: " + response.error + L".");
                        }
                        if (is_latest() && response.error != L"canceled")
                        {
                            publish_gallery_response(context, std::move(response));
                        }
                    }
                }

                std::optional<std::wstring> source_status_language;
                {
                    const std::scoped_lock lock(context->gallery_mutex);
                    source_status_language = std::move(context->source_status_language);
                    context->source_status_language.reset();
                }
                if (source_status_language)
                {
                    auto payload = make_source_status_payload(
                        selection_service.source_statuses(*source_status_language));
                    {
                        const std::scoped_lock lock(context->gallery_mutex);
                        context->source_status_responses.push_back(std::move(payload));
                    }
                    const HWND window = context->window.load(std::memory_order_acquire);
                    if (!context->source_status_result_pending.exchange(
                            true, std::memory_order_acq_rel) &&
                        (window == nullptr || !PostMessageW(
                            window, context->source_status_result_message, 0, 0)))
                    {
                        context->source_status_result_pending.store(
                            false, std::memory_order_release);
                    }
                }

                const auto query_started = std::chrono::steady_clock::now();
                auto next = selection_service.query_foreground();
                const bool suppress_preview_update =
                    selection_service.consume_gallery_selection_sync(next);
                const auto query_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - query_started);
                if (context->generation.load(std::memory_order_acquire) != generation ||
                    WaitForSingleObject(context->stop_event.get(), 0) != WAIT_TIMEOUT)
                {
                    break;
                }

                next.timestamp_ms = GetTickCount64();
                context->progress_ms.store(next.timestamp_ms, std::memory_order_release);
                context->completed_generation.store(generation, std::memory_order_release);
                if (query_duration >= std::chrono::milliseconds(100))
                {
                    glance::contracts::log_event(
                        L"Explorer selection query took " +
                        std::to_wstring(query_duration.count()) + L" ms.");
                }
                publish_selection(
                    context,
                    generation,
                    std::move(next),
                    suppress_preview_update);

                const HANDLE events[]{ context->stop_event.get(), context->gallery_event.get() };
                const DWORD wait_result = MsgWaitForMultipleObjectsEx(
                    static_cast<DWORD>(std::size(events)),
                    events,
                    selection_interval_ms,
                    QS_ALLINPUT,
                    MWMO_INPUTAVAILABLE);
                if (wait_result == WAIT_OBJECT_0)
                {
                    break;
                }
                if (wait_result == WAIT_OBJECT_0 + std::size(events))
                {
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                    {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                }
            }
        }
        catch (const std::exception&)
        {
            if (context->generation.load(std::memory_order_acquire) == generation)
            {
                glance::contracts::log_event(L"Selection worker stopped after a standard exception.");
            }
        }
        catch (...)
        {
            if (context->generation.load(std::memory_order_acquire) == generation)
            {
                glance::contracts::log_event(L"Selection worker stopped after an unknown exception.");
            }
        }

        CoUninitialize();
    }

    void CoreApplication::publish_selection(
        const std::shared_ptr<SelectionWorkerContext>& context,
        std::uint64_t generation,
        glance::contracts::SelectionSnapshot next,
        bool suppress_preview_update)
    {
        {
            const std::scoped_lock lock(context->pending_mutex);
            if (context->generation.load(std::memory_order_acquire) != generation)
            {
                return;
            }
            const bool same_pending_selection = context->pending_selection &&
                context->pending_selection->source_window == next.source_window &&
                context->pending_selection->focused_index < context->pending_selection->items.size() &&
                next.focused_index < next.items.size() &&
                paths_equal(
                    context->pending_selection->items[context->pending_selection->focused_index]
                        .filesystem_path,
                    next.items[next.focused_index].filesystem_path);
            const bool keep_suppressed = same_pending_selection &&
                context->pending_selection_suppressed;
            context->pending_selection = std::move(next);
            context->pending_generation = generation;
            context->pending_selection_suppressed = keep_suppressed || suppress_preview_update;
        }
        const HWND window = context->window.load(std::memory_order_acquire);
        if (!context->message_pending.exchange(true, std::memory_order_acq_rel) &&
            (window == nullptr || !PostMessageW(window, context->result_message, 0, 0)))
        {
            context->message_pending.store(false, std::memory_order_release);
        }
    }

    void CoreApplication::publish_gallery_response(
        const std::shared_ptr<SelectionWorkerContext>& context,
        GalleryResponse response)
    {
        {
            const std::scoped_lock lock(context->gallery_mutex);
            context->gallery_responses.push_back(std::move(response));
        }
        const HWND window = context->window.load(std::memory_order_acquire);
        if (!context->gallery_result_pending.exchange(true, std::memory_order_acq_rel) &&
            (window == nullptr || !PostMessageW(window, context->gallery_result_message, 0, 0)))
        {
            context->gallery_result_pending.store(false, std::memory_order_release);
        }
    }

    void CoreApplication::stop_selection_worker() noexcept
    {
        const auto context = selection_worker_context_;
        if (context)
        {
            context->generation.fetch_add(1, std::memory_order_acq_rel);
            context->window.store(nullptr, std::memory_order_release);
            SetEvent(context->stop_event.get());
        }
        if (selection_worker_.joinable())
        {
            selection_worker_.detach();
        }
        selection_worker_context_.reset();
    }

    void CoreApplication::monitor_selection_worker()
    {
        const auto context = selection_worker_context_;
        if (!context)
        {
            return;
        }
        const auto now = GetTickCount64();
        const auto generation = context->generation.load(std::memory_order_acquire);
        const auto completed_generation = context->completed_generation.load(std::memory_order_acquire);
        const auto progress_ms = context->progress_ms.load(std::memory_order_acquire);
        if (completed_generation == generation &&
            progress_ms != 0 &&
            now - progress_ms <= selection_worker_stall_after_ms)
        {
            selection_worker_consecutive_stalls_ = 0;
            return;
        }
        if (now - selection_worker_last_restart_ms_ <= selection_worker_stall_after_ms)
        {
            return;
        }
        if (now < selection_worker_cooldown_until_ms_)
        {
            return;
        }
        if (++selection_worker_consecutive_stalls_ >= selection_worker_max_restarts)
        {
            selection_worker_cooldown_until_ms_ =
                now + selection_worker_cooldown_ms;
            selection_worker_consecutive_stalls_ = 0;
            glance::contracts::log_event(
                L"Selection worker stalled repeatedly; pausing restart for " +
                std::to_wstring(selection_worker_cooldown_ms) + L" ms.");
            return;
        }

        input_state_.eligible_selection.store(false, std::memory_order_release);
        const auto replacement_generation = context->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (selection_worker_.joinable())
        {
            selection_worker_.detach();
        }
        selection_worker_last_restart_ms_ = now;
        glance::contracts::log_event(
            L"Selection worker stalled; starting replacement generation " +
            std::to_wstring(replacement_generation) + L".");
        if (!start_selection_worker_generation())
        {
            glance::contracts::log_event(L"Failed to start the replacement selection worker.");
        }
    }

    bool CoreApplication::selection_worker_healthy() const noexcept
    {
        const auto context = selection_worker_context_;
        if (!context)
        {
            return false;
        }
        const auto generation = context->generation.load(std::memory_order_acquire);
        const auto completed_generation = context->completed_generation.load(std::memory_order_acquire);
        const auto progress_ms = context->progress_ms.load(std::memory_order_acquire);
        return completed_generation == generation &&
            progress_ms != 0 &&
            GetTickCount64() - progress_ms <= selection_worker_stall_after_ms;
    }

    void CoreApplication::apply_pending_selection()
    {
        const auto context = selection_worker_context_;
        if (!context)
        {
            return;
        }
        std::optional<glance::contracts::SelectionSnapshot> next;
        std::uint64_t generation{};
        bool suppress_preview_update{};
        {
            const std::scoped_lock lock(context->pending_mutex);
            next = std::move(context->pending_selection);
            context->pending_selection.reset();
            generation = context->pending_generation;
            suppress_preview_update = context->pending_selection_suppressed;
            context->pending_selection_suppressed = false;
            context->message_pending.store(false, std::memory_order_release);
        }
        if (next && generation == context->generation.load(std::memory_order_acquire))
        {
            apply_selection(std::move(*next), suppress_preview_update);
        }
    }

    void CoreApplication::apply_pending_gallery_commands()
    {
        std::vector<std::string> payloads;
        {
            const std::scoped_lock lock(gallery_payload_mutex_);
            payloads.swap(pending_gallery_payloads_);
        }
        const auto context = selection_worker_context_;
        if (!context)
        {
            return;
        }
        bool queued{};
        for (const auto& payload : payloads)
        {
            auto command = parse_gallery_command(payload);
            if (!command)
            {
                glance::contracts::log_event(L"Ignoring an invalid gallery request payload.");
                continue;
            }
            {
                const std::scoped_lock lock(context->gallery_mutex);
                context->latest_gallery_requests[command->window_id] = command->request_id;
                std::erase_if(context->gallery_commands, [window_id = command->window_id](const auto& pending) {
                    return pending.window_id == window_id;
                });
                context->gallery_commands.push_back(std::move(*command));
            }
            queued = true;
        }
        if (queued)
        {
            SetEvent(context->gallery_event.get());
        }
    }

    void CoreApplication::apply_pending_gallery_responses()
    {
        const auto context = selection_worker_context_;
        if (!context)
        {
            return;
        }
        std::deque<GalleryResponse> responses;
        {
            const std::scoped_lock lock(context->gallery_mutex);
            responses.swap(context->gallery_responses);
            for (const auto& response : responses)
            {
                if (response.operation != GalleryOperation::close)
                {
                    continue;
                }
                const auto latest = context->latest_gallery_requests.find(response.window_id);
                if (latest != context->latest_gallery_requests.end() &&
                    latest->second == response.request_id)
                {
                    context->latest_gallery_requests.erase(latest);
                }
            }
            context->gallery_result_pending.store(false, std::memory_order_release);
        }
        for (const auto& response : responses)
        {
            static_cast<void>(pipe_server_.send(
                glance::contracts::MessageType::gallery_response,
                make_gallery_response_payload(response)));
        }
    }

    void CoreApplication::apply_pending_source_status_responses()
    {
        const auto context = selection_worker_context_;
        if (!context)
        {
            return;
        }
        std::deque<std::string> responses;
        {
            const std::scoped_lock lock(context->gallery_mutex);
            responses.swap(context->source_status_responses);
            context->source_status_result_pending.store(false, std::memory_order_release);
        }
        for (const auto& response : responses)
        {
            static_cast<void>(pipe_server_.send(
                glance::contracts::MessageType::source_status_response,
                response));
        }
    }

    void CoreApplication::apply_selection(
        glance::contracts::SelectionSnapshot next,
        bool suppress_preview_update)
    {
        input_state_.text_input_active.store(next.text_input_active, std::memory_order_release);

        auto preview_state = preview_state_.load(std::memory_order_acquire);
        const HWND foreground_window = GetForegroundWindow();
        DWORD foreground_process_id{};
        if (foreground_window != nullptr)
        {
            GetWindowThreadProcessId(foreground_window, &foreground_process_id);
        }
        const bool app_is_foreground =
            foreground_window != nullptr &&
            foreground_process_id != 0 &&
            app_process_ &&
            foreground_process_id == GetProcessId(app_process_.get());
        if (preview_state == glance::contracts::PreviewWindowState::active_interactive ||
            (preview_state == glance::contracts::PreviewWindowState::active_following &&
             app_is_foreground))
        {
            input_state_.eligible_selection.store(false, std::memory_order_release);
            return;
        }
        if (preview_state == glance::contracts::PreviewWindowState::active_following &&
            selection_.source_window != 0 &&
            reinterpret_cast<std::uintptr_t>(foreground_window) != selection_.source_window)
        {
            if (pipe_server_.send(glance::contracts::MessageType::close_active_preview))
            {
                preview_state_.store(
                    glance::contracts::PreviewWindowState::hidden,
                    std::memory_order_release);
                input_state_.preview_active.store(false, std::memory_order_release);
                preview_state = glance::contracts::PreviewWindowState::hidden;
            }
        }
        const bool changed = selection_changed(next);
        if (changed)
        {
            if (next.host_kind != glance::contracts::HostKind::unsupported)
            {
                glance::contracts::log_event(
                    L"Selection state: host=" +
                    std::to_wstring(static_cast<unsigned>(next.host_kind)) +
                    L", eligible=" + std::to_wstring(next.accepts_hotkey) +
                    L", items=" + std::to_wstring(next.items.size()) + L".");
            }
            next.generation = ++selection_generation_;
            selection_ = std::move(next);
        }
        else
        {
            selection_.timestamp_ms = next.timestamp_ms;
        }

        const bool eligible = selection_.accepts_hotkey &&
                              selection_.host_kind != glance::contracts::HostKind::unsupported &&
                              !selection_.items.empty();
        input_state_.eligible_selection.store(
            eligible && preview_state != glance::contracts::PreviewWindowState::active_interactive,
            std::memory_order_release);

        const bool follows_selection = preview_state == glance::contracts::PreviewWindowState::active_following ||
                                       preview_state == glance::contracts::PreviewWindowState::active_topmost;
        if (!changed || !follows_selection || suppress_preview_update)
        {
            return;
        }
        if (eligible)
        {
            static_cast<void>(pipe_server_.send(
                glance::contracts::MessageType::open_active_preview,
                make_open_payload(selection_)));
        }
        else if (preview_state == glance::contracts::PreviewWindowState::active_following &&
                 !app_is_foreground)
        {
            if (pipe_server_.send(glance::contracts::MessageType::close_active_preview))
            {
                preview_state_.store(
                    glance::contracts::PreviewWindowState::hidden,
                    std::memory_order_release);
                input_state_.preview_active.store(false, std::memory_order_release);
            }
        }
    }

    bool CoreApplication::selection_changed(const glance::contracts::SelectionSnapshot& next) const
    {
        if (selection_.source_window != next.source_window ||
            selection_.host_kind != next.host_kind ||
            selection_.source_id != next.source_id ||
            selection_.source_capabilities != next.source_capabilities ||
            selection_.accepts_hotkey != next.accepts_hotkey ||
            selection_.text_input_active != next.text_input_active ||
            selection_.focused_index != next.focused_index ||
            selection_.items.size() != next.items.size())
        {
            return true;
        }
        for (std::size_t index = 0; index < next.items.size(); ++index)
        {
            if (selection_.items[index].filesystem_path != next.items[index].filesystem_path ||
                selection_.items[index].shell_parsing_name != next.items[index].shell_parsing_name)
            {
                return true;
            }
        }
        return false;
    }

    void CoreApplication::handle_hook_action(HookAction action, std::uint64_t posted_at_ms)
    {
        const auto dispatch_delay = GetTickCount64() - posted_at_ms;
        if (dispatch_delay >= 50)
        {
            glance::contracts::log_event(
                L"Hook action dispatch was delayed by " + std::to_wstring(dispatch_delay) + L" ms.");
        }

        if (!input_state_.ui_connected.load(std::memory_order_acquire))
        {
            return;
        }

        if (input_state_.preview_active.load(std::memory_order_acquire))
        {
            const auto input_action = action == HookAction::toggle_preview
                ? glance::contracts::PreviewInputAction::activate_selection
                : glance::contracts::PreviewInputAction::navigate_back;
            static_cast<void>(pipe_server_.send(
                glance::contracts::MessageType::preview_input,
                {},
                static_cast<std::uint32_t>(input_action)));
            return;
        }

        if (action == HookAction::close_preview)
        {
            return;
        }

        const HWND foreground = GetForegroundWindow();
        const HWND foreground_root = foreground == nullptr ? nullptr : GetAncestor(foreground, GA_ROOT);
        if (selection_.items.empty() ||
            selection_.source_window != reinterpret_cast<std::uintptr_t>(foreground_root) ||
            GetTickCount64() - selection_.timestamp_ms > selection_stale_after_ms)
        {
            return;
        }
        const auto payload = make_open_payload(selection_);
        if (pipe_server_.send(glance::contracts::MessageType::open_active_preview, payload))
        {
            preview_state_.store(
                glance::contracts::PreviewWindowState::active_following,
                std::memory_order_release);
            input_state_.preview_active.store(true, std::memory_order_release);
        }
    }

    void CoreApplication::handle_pipe_message(
        glance::contracts::MessageType type,
        std::uint32_t flags,
        std::string_view payload)
    {
        if (type == glance::contracts::MessageType::terminate_unresponsive)
        {
            glance::contracts::log_event(L"UI requested emergency Core termination.");
            TerminateProcess(GetCurrentProcess(), ERROR_PROCESS_ABORTED);
            return;
        }
        if (type == glance::contracts::MessageType::heartbeat_ack)
        {
            auto acknowledged = last_heartbeat_ack_.load(std::memory_order_relaxed);
            while (flags > acknowledged &&
                   !last_heartbeat_ack_.compare_exchange_weak(
                       acknowledged,
                       flags,
                       std::memory_order_release,
                       std::memory_order_relaxed))
            {
            }
            return;
        }
        if (type == glance::contracts::MessageType::heartbeat)
        {
            PostMessageW(window_, heartbeat_message, static_cast<WPARAM>(flags), 0);
            return;
        }
        if (type == glance::contracts::MessageType::hello)
        {
            static_cast<void>(pipe_server_.send(glance::contracts::MessageType::hello_ack));
            return;
        }
        if (type == glance::contracts::MessageType::preview_state_changed)
        {
            const auto state = static_cast<glance::contracts::PreviewWindowState>(flags);
            preview_state_.store(state, std::memory_order_release);
            const bool interactive =
                state == glance::contracts::PreviewWindowState::active_interactive;
            const bool active = state == glance::contracts::PreviewWindowState::active_following ||
                                state == glance::contracts::PreviewWindowState::active_topmost ||
                                state == glance::contracts::PreviewWindowState::active_pinned;
            input_state_.preview_active.store(active, std::memory_order_release);
            if (interactive)
            {
                input_state_.eligible_selection.store(false, std::memory_order_release);
            }
            return;
        }
        if (type == glance::contracts::MessageType::gallery_request)
        {
            {
                const std::scoped_lock lock(gallery_payload_mutex_);
                pending_gallery_payloads_.emplace_back(payload);
            }
            PostMessageW(window_, gallery_command_message, 0, 0);
            return;
        }
        if (type == glance::contracts::MessageType::source_status_request)
        {
            glance::contracts::log_event(L"Optional source status query requested.");
            const auto context = selection_worker_context_;
            if (context)
            {
                {
                    const std::scoped_lock lock(context->gallery_mutex);
                    context->source_status_language = winrt::to_hstring(payload).c_str();
                }
                SetEvent(context->gallery_event.get());
            }
            return;
        }
        if (type == glance::contracts::MessageType::shutdown)
        {
            shutting_down_.store(true, std::memory_order_release);
            PostMessageW(window_, WM_CLOSE, 0, 0);
        }
    }

    void CoreApplication::handle_connection_changed(bool connected)
    {
        glance::contracts::log_event(connected ? L"UI pipe connected." : L"UI pipe disconnected.");
        input_state_.ui_connected.store(false, std::memory_order_release);
        if (!connected)
        {
            preview_state_.store(
                glance::contracts::PreviewWindowState::hidden,
                std::memory_order_release);
            input_state_.preview_active.store(false, std::memory_order_release);
        }
        PostMessageW(
            window_,
            connection_changed_message,
            connected ? 1U : 0U,
            static_cast<LPARAM>(pipe_server_.peer_process_id()));
    }

    std::string CoreApplication::make_open_payload(
        const glance::contracts::SelectionSnapshot& selection) const
    {
        using namespace winrt::Windows::Data::Json;
        constexpr std::size_t maximum_payload_files = 512;
        JsonObject root;
        JsonArray files;
        const auto file_count = std::min<std::size_t>(
            selection.items.size(),
            maximum_payload_files);
        if (selection.items.size() > file_count)
        {
            glance::contracts::log_event(
                L"Truncating a large selection to the first " +
                std::to_wstring(file_count) + L" files for the preview payload.");
        }
        for (std::size_t index = 0; index < file_count; ++index)
        {
            const auto& item = selection.items[index];
            JsonObject file;
            file.SetNamedValue(L"displayName", JsonValue::CreateStringValue(item.display_name));
            file.SetNamedValue(L"path", JsonValue::CreateStringValue(item.filesystem_path));
            file.SetNamedValue(L"parsingName", JsonValue::CreateStringValue(item.shell_parsing_name));
            file.SetNamedValue(L"size", JsonValue::CreateStringValue(std::to_wstring(item.size)));
            file.SetNamedValue(L"creationTime", JsonValue::CreateStringValue(std::to_wstring(item.creation_time)));
            file.SetNamedValue(L"lastWriteTime", JsonValue::CreateStringValue(std::to_wstring(item.last_write_time)));
            file.SetNamedValue(L"attributes", JsonValue::CreateNumberValue(item.attributes));
            file.SetNamedValue(L"isFilesystem", JsonValue::CreateBooleanValue(item.is_filesystem));
            file.SetNamedValue(L"isCloudPlaceholder", JsonValue::CreateBooleanValue(item.is_cloud_placeholder));
            files.Append(file);
        }
        root.SetNamedValue(L"files", files);
        root.SetNamedValue(L"focusedIndex", JsonValue::CreateNumberValue(selection.focused_index));
        root.SetNamedValue(L"sourceKind", JsonValue::CreateNumberValue(static_cast<int>(selection.host_kind)));
        root.SetNamedValue(
            L"sourceWindow",
            JsonValue::CreateStringValue(std::to_wstring(selection.source_window)));
        root.SetNamedValue(L"sourceId", JsonValue::CreateStringValue(selection.source_id));
        root.SetNamedValue(
            L"sourceCapabilities",
            JsonValue::CreateStringValue(std::to_wstring(selection.source_capabilities)));
        root.SetNamedValue(L"generation", JsonValue::CreateStringValue(std::to_wstring(selection.generation)));
        return winrt::to_string(root.Stringify());
    }
}

#include "core_application.h"
#include "explorer_selection.h"
#include "glance/contracts/diagnostics.h"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <ranges>
#include <wtsapi32.h>

namespace
{
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
}

namespace glance::core
{
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

    int CoreApplication::run(HINSTANCE instance, DWORD parent_process_id)
    {
        single_instance_mutex_.reset(CreateMutexW(nullptr, FALSE, L"Local\\Glance.Core"));
        if (!single_instance_mutex_ || GetLastError() == ERROR_ALREADY_EXISTS)
        {
            return 0;
        }
        if (process_is_elevated())
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

        if (parent_process_id != 0)
        {
            parent_process_.reset(OpenProcess(SYNCHRONIZE, FALSE, parent_process_id));
            if (parent_process_)
            {
                SetTimer(window_, parent_process_timer_id, parent_process_interval_ms, nullptr);
            }
            else
            {
                glance::contracts::log_event(
                    L"Could not monitor the UI process " + std::to_wstring(parent_process_id) + L".");
            }
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
            if (wparam == parent_process_timer_id && self->parent_process_ &&
                WaitForSingleObject(self->parent_process_.get(), 0) == WAIT_OBJECT_0)
            {
                glance::contracts::log_event(L"UI process exited; Core is shutting down.");
                DestroyWindow(window);
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

    bool CoreApplication::start_selection_worker()
    {
        selection_stop_event_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!selection_stop_event_)
        {
            return false;
        }

        try
        {
            selection_worker_ = std::thread([this] {
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
                    while (WaitForSingleObject(selection_stop_event_.get(), 0) == WAIT_TIMEOUT)
                    {
                        const auto query_started = std::chrono::steady_clock::now();
                        auto next = selection_service.query_foreground();
                        const auto query_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - query_started);
                        next.timestamp_ms = GetTickCount64();
                        if (query_duration >= std::chrono::milliseconds(100))
                        {
                            glance::contracts::log_event(
                                L"Explorer selection query took " +
                                std::to_wstring(query_duration.count()) + L" ms.");
                        }
                        publish_selection(std::move(next));

                        const HANDLE stop_event = selection_stop_event_.get();
                        const DWORD wait_result = MsgWaitForMultipleObjectsEx(
                            1,
                            &stop_event,
                            selection_interval_ms,
                            QS_ALLINPUT,
                            MWMO_INPUTAVAILABLE);
                        if (wait_result == WAIT_OBJECT_0)
                        {
                            break;
                        }
                        if (wait_result == WAIT_OBJECT_0 + 1)
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
                    glance::contracts::log_event(L"Selection worker stopped after a standard exception.");
                }
                catch (...)
                {
                    glance::contracts::log_event(L"Selection worker stopped after an unknown exception.");
                }

                CoUninitialize();
            });
        }
        catch (...)
        {
            selection_stop_event_.reset();
            return false;
        }
        return true;
    }

    void CoreApplication::stop_selection_worker() noexcept
    {
        if (selection_stop_event_)
        {
            SetEvent(selection_stop_event_.get());
        }
        if (selection_worker_.joinable())
        {
            selection_worker_.join();
        }
        selection_stop_event_.reset();
    }

    void CoreApplication::publish_selection(glance::contracts::SelectionSnapshot next)
    {
        {
            const std::scoped_lock lock(pending_selection_mutex_);
            pending_selection_ = std::move(next);
        }
        if (!selection_message_pending_.exchange(true, std::memory_order_acq_rel) &&
            !PostMessageW(window_, selection_result_message, 0, 0))
        {
            selection_message_pending_.store(false, std::memory_order_release);
        }
    }

    void CoreApplication::apply_pending_selection()
    {
        std::optional<glance::contracts::SelectionSnapshot> next;
        {
            const std::scoped_lock lock(pending_selection_mutex_);
            next = std::move(pending_selection_);
            pending_selection_.reset();
            selection_message_pending_.store(false, std::memory_order_release);
        }
        if (next)
        {
            apply_selection(std::move(*next));
        }
    }

    void CoreApplication::apply_selection(glance::contracts::SelectionSnapshot next)
    {
        auto preview_state = preview_state_.load(std::memory_order_acquire);
        if (preview_state == glance::contracts::PreviewWindowState::active_following &&
            selection_.source_window != 0 &&
            reinterpret_cast<std::uintptr_t>(GetForegroundWindow()) != selection_.source_window)
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
        input_state_.eligible_selection.store(eligible, std::memory_order_release);

        const bool follows_selection = preview_state == glance::contracts::PreviewWindowState::active_following ||
                                       preview_state == glance::contracts::PreviewWindowState::active_topmost;
        if (!changed || !follows_selection)
        {
            return;
        }
        if (eligible)
        {
            static_cast<void>(pipe_server_.send(
                glance::contracts::MessageType::open_active_preview,
                make_open_payload(selection_)));
        }
        else if (preview_state == glance::contracts::PreviewWindowState::active_following)
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
            selection_.accepts_hotkey != next.accepts_hotkey ||
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
            if (pipe_server_.send(glance::contracts::MessageType::close_active_preview))
            {
                preview_state_.store(
                    glance::contracts::PreviewWindowState::hidden,
                    std::memory_order_release);
                input_state_.preview_active.store(false, std::memory_order_release);
            }
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
        std::string_view)
    {
        if (type == glance::contracts::MessageType::hello)
        {
            static_cast<void>(pipe_server_.send(glance::contracts::MessageType::hello_ack));
            return;
        }
        if (type == glance::contracts::MessageType::preview_state_changed)
        {
            const auto state = static_cast<glance::contracts::PreviewWindowState>(flags);
            preview_state_.store(state, std::memory_order_release);
            const bool active = state == glance::contracts::PreviewWindowState::active_following ||
                                state == glance::contracts::PreviewWindowState::active_topmost ||
                                state == glance::contracts::PreviewWindowState::active_pinned;
            input_state_.preview_active.store(active, std::memory_order_release);
            return;
        }
        if (type == glance::contracts::MessageType::shutdown)
        {
            PostMessageW(window_, WM_CLOSE, 0, 0);
        }
    }

    void CoreApplication::handle_connection_changed(bool connected)
    {
        glance::contracts::log_event(connected ? L"UI pipe connected." : L"UI pipe disconnected.");
        input_state_.ui_connected.store(connected, std::memory_order_release);
        if (!connected)
        {
            preview_state_.store(
                glance::contracts::PreviewWindowState::hidden,
                std::memory_order_release);
            input_state_.preview_active.store(false, std::memory_order_release);
            PostMessageW(window_, WM_CLOSE, 0, 0);
        }
    }

    std::string CoreApplication::make_open_payload(
        const glance::contracts::SelectionSnapshot& selection) const
    {
        using namespace winrt::Windows::Data::Json;
        JsonObject root;
        JsonArray files;
        for (const auto& item : selection.items)
        {
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
        root.SetNamedValue(L"generation", JsonValue::CreateStringValue(std::to_wstring(selection.generation)));
        return winrt::to_string(root.Stringify());
    }
}

#include "keyboard_hook.h"
#include "input_decision.h"
#include "glance/contracts/diagnostics.h"

#include <string>

namespace glance::core
{
    thread_local KeyboardHookService::HookThread* KeyboardHookService::HookThread::current_ = nullptr;

    KeyboardHookService::KeyboardHookService(
        HWND notification_window,
        UINT notification_message,
        InputDecisionState& state) noexcept
        : notification_window_(notification_window),
          notification_message_(notification_message),
          state_(state),
          standby_(*this),
          primary_(*this)
    {
    }

    KeyboardHookService::~KeyboardHookService()
    {
        stop();
    }

    bool KeyboardHookService::start()
    {
        if (!standby_.start())
        {
            return false;
        }
        if (!primary_.start())
        {
            standby_.stop();
            return false;
        }
        return true;
    }

    void KeyboardHookService::stop() noexcept
    {
        primary_.stop();
        standby_.stop();
    }

    bool KeyboardHookService::refresh()
    {
        space_down_.store(false, std::memory_order_release);
        space_captured_.store(false, std::memory_order_release);
        escape_down_.store(false, std::memory_order_release);
        escape_captured_.store(false, std::memory_order_release);
        standby_.stop();
        if (!standby_.start())
        {
            return false;
        }
        primary_.stop();
        return primary_.start();
    }

    bool KeyboardHookService::handle_key(WPARAM message, const KBDLLHOOKSTRUCT& key) noexcept
    {
        const bool key_down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        const bool key_up = message == WM_KEYUP || message == WM_SYSKEYUP;
        if (!key_down && !key_up)
        {
            return false;
        }

        std::atomic_bool* down = nullptr;
        std::atomic_bool* captured = nullptr;
        HookAction action{};

        if (key.vkCode == VK_SPACE)
        {
            down = &space_down_;
            captured = &space_captured_;
            action = HookAction::toggle_preview;
        }
        else if (key.vkCode == VK_ESCAPE)
        {
            down = &escape_down_;
            captured = &escape_captured_;
            action = HookAction::close_preview;
        }
        else
        {
            return false;
        }

        if (key_down)
        {
            if (down->exchange(true, std::memory_order_acq_rel))
            {
                return captured->load(std::memory_order_acquire);
            }

            const bool connected = state_.ui_connected.load(std::memory_order_acquire);
            const bool active = state_.preview_active.load(std::memory_order_acquire);
            const bool eligible = state_.eligible_selection.load(std::memory_order_acquire);
            const bool modified = (key.flags & LLKHF_ALTDOWN) != 0 ||
                (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 ||
                (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 ||
                (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
                (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
            const bool should_capture = should_capture_key(
                key.vkCode,
                connected,
                active,
                eligible,
                modified);

            if (key.vkCode == VK_SPACE)
            {
                glance::contracts::log_event(
                    L"Space input decision: connected=" + std::to_wstring(connected) +
                    L", active=" + std::to_wstring(active) +
                    L", eligible=" + std::to_wstring(eligible) +
                    L", modified=" + std::to_wstring(modified) +
                    L", capture=" + std::to_wstring(should_capture) + L".");
            }

            captured->store(should_capture, std::memory_order_release);
            if (should_capture)
            {
                PostMessageW(
                    notification_window_,
                    notification_message_,
                    static_cast<WPARAM>(action),
                    static_cast<LPARAM>(GetTickCount64()));
            }
            return should_capture;
        }

        const bool should_capture = captured->exchange(false, std::memory_order_acq_rel);
        down->store(false, std::memory_order_release);
        return should_capture;
    }

    KeyboardHookService::HookThread::~HookThread()
    {
        stop();
    }

    bool KeyboardHookService::HookThread::start()
    {
        if (thread_.joinable())
        {
            return start_succeeded_;
        }

        ready_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (ready_event_ == nullptr)
        {
            return false;
        }

        thread_ = std::thread([this] { run(); });
        WaitForSingleObject(ready_event_, INFINITE);
        CloseHandle(ready_event_);
        ready_event_ = nullptr;
        return start_succeeded_;
    }

    void KeyboardHookService::HookThread::stop() noexcept
    {
        if (!thread_.joinable())
        {
            return;
        }
        PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
        thread_.join();
        thread_id_ = 0;
        start_succeeded_ = false;
    }

    LRESULT CALLBACK KeyboardHookService::HookThread::hook_proc(
        int code,
        WPARAM message,
        LPARAM data) noexcept
    {
        if (code == HC_ACTION && current_ != nullptr)
        {
            current_->owner_.event_count_.fetch_add(1, std::memory_order_relaxed);
            const auto& key = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
            if (current_->owner_.handle_key(message, key))
            {
                return 1;
            }
        }
        return CallNextHookEx(nullptr, code, message, data);
    }

    void KeyboardHookService::HookThread::run() noexcept
    {
        current_ = this;
        thread_id_ = GetCurrentThreadId();
        MSG message{};
        PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

        hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, hook_proc, GetModuleHandleW(nullptr), 0);
        start_succeeded_ = hook_ != nullptr;
        if (!start_succeeded_)
        {
            glance::contracts::log_event(
                L"SetWindowsHookExW failed with error " + std::to_wstring(GetLastError()) + L".");
        }
        SetEvent(ready_event_);

        if (start_succeeded_)
        {
            while (GetMessageW(&message, nullptr, 0, 0) > 0)
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            UnhookWindowsHookEx(hook_);
            hook_ = nullptr;
        }
        current_ = nullptr;
    }
}

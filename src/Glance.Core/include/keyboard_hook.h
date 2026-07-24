#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <thread>

namespace glance::core
{
    enum class HookAction : WPARAM
    {
        toggle_preview = 1,
        close_preview = 2,
    };

    struct InputDecisionState
    {
        std::atomic_bool ui_connected{};
        std::atomic_bool eligible_selection{};
        std::atomic_bool preview_active{};
        std::atomic_bool text_input_active{};
    };

    class KeyboardHookService
    {
    public:
        KeyboardHookService(HWND notification_window, UINT notification_message, InputDecisionState& state) noexcept;
        ~KeyboardHookService();

        KeyboardHookService(const KeyboardHookService&) = delete;
        KeyboardHookService& operator=(const KeyboardHookService&) = delete;

        [[nodiscard]] bool start();
        [[nodiscard]] bool refresh();
        [[nodiscard]] std::uint64_t event_count() const noexcept
        {
            return event_count_.load(std::memory_order_relaxed);
        }
        void stop() noexcept;

    private:
        class HookThread
        {
        public:
            explicit HookThread(KeyboardHookService& owner) noexcept : owner_(owner) {}
            ~HookThread();

            [[nodiscard]] bool start();
            void stop() noexcept;

        private:
            static LRESULT CALLBACK hook_proc(int code, WPARAM message, LPARAM data) noexcept;
            void run() noexcept;

            KeyboardHookService& owner_;
            std::thread thread_;
            DWORD thread_id_{};
            HHOOK hook_{};
            HANDLE ready_event_{};
            bool start_succeeded_{};
            static thread_local HookThread* current_;
        };

        [[nodiscard]] bool handle_key(WPARAM message, const KBDLLHOOKSTRUCT& key) noexcept;

        HWND notification_window_{};
        UINT notification_message_{};
        InputDecisionState& state_;
        HookThread standby_;
        HookThread primary_;
        std::atomic_bool space_down_{};
        std::atomic_bool space_captured_{};
        std::atomic_bool escape_down_{};
        std::atomic_bool escape_captured_{};
        std::atomic_uint64_t event_count_{};
    };
}

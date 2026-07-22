#pragma once

#include "keyboard_hook.h"
#include "pipe_server.h"
#include "unique_handle.h"

#include "glance/contracts/file_descriptor.h"
#include "glance/contracts/preview_state.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace glance::core
{
    class CoreApplication
    {
    public:
        CoreApplication();
        ~CoreApplication();

        CoreApplication(const CoreApplication&) = delete;
        CoreApplication& operator=(const CoreApplication&) = delete;

        [[nodiscard]] int run(HINSTANCE instance, DWORD parent_process_id);

    private:
        static constexpr UINT hook_action_message = WM_APP + 1;
        static constexpr UINT selection_result_message = WM_APP + 2;
        static constexpr UINT selection_timer_id = 1;
        static constexpr UINT hook_refresh_timer_id = 2;
        static constexpr UINT parent_process_timer_id = 3;
        static constexpr UINT selection_interval_ms = 25;
        static constexpr UINT selection_stale_after_ms = 500;
        static constexpr UINT hook_refresh_interval_ms = 1000;
        static constexpr UINT parent_process_interval_ms = 1000;

        static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
        [[nodiscard]] bool create_message_window(HINSTANCE instance);
        [[nodiscard]] bool start_selection_worker();
        void stop_selection_worker() noexcept;
        void publish_selection(glance::contracts::SelectionSnapshot next);
        void apply_pending_selection();
        void apply_selection(glance::contracts::SelectionSnapshot next);
        void handle_hook_action(HookAction action, std::uint64_t posted_at_ms);
        void handle_pipe_message(glance::contracts::MessageType type, std::uint32_t flags, std::string_view payload);
        void handle_connection_changed(bool connected);
        void recover_keyboard_hook(std::wstring_view reason);
        [[nodiscard]] std::string make_open_payload(const glance::contracts::SelectionSnapshot& selection) const;
        [[nodiscard]] bool selection_changed(const glance::contracts::SelectionSnapshot& next) const;

        HWND window_{};
        unique_handle single_instance_mutex_;
        unique_handle elevated_status_mutex_;
        unique_handle parent_process_;
        InputDecisionState input_state_;
        glance::contracts::SelectionSnapshot selection_;
        unique_handle selection_stop_event_;
        std::thread selection_worker_;
        std::mutex pending_selection_mutex_;
        std::optional<glance::contracts::SelectionSnapshot> pending_selection_;
        std::atomic_bool selection_message_pending_{};
        std::uint64_t selection_generation_{};
        PipeServer pipe_server_;
        KeyboardHookService* keyboard_hook_{};
        std::atomic_uint64_t raw_input_count_{};
        std::uint64_t previous_raw_input_count_{};
        std::uint64_t previous_hook_event_count_{};
        std::atomic<glance::contracts::PreviewWindowState> preview_state_{
            glance::contracts::PreviewWindowState::hidden };
    };
}

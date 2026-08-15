#pragma once

#include "keyboard_hook.h"
#include "network_service.h"
#include "pipe_server.h"
#include "unique_handle.h"

#include "glance/contracts/file_descriptor.h"
#include "glance/contracts/preview_state.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace glance::core
{
    struct SelectionWorkerContext;
    struct GalleryResponse;

    class CoreApplication
    {
    public:
        CoreApplication();
        ~CoreApplication();

        CoreApplication(const CoreApplication&) = delete;
        CoreApplication& operator=(const CoreApplication&) = delete;

        [[nodiscard]] int run(HINSTANCE instance, DWORD app_process_id);

    private:
        static constexpr UINT hook_action_message = WM_APP + 1;
        static constexpr UINT selection_result_message = WM_APP + 2;
        static constexpr UINT heartbeat_message = WM_APP + 3;
        static constexpr UINT connection_changed_message = WM_APP + 4;
        static constexpr UINT gallery_command_message = WM_APP + 5;
        static constexpr UINT gallery_result_message = WM_APP + 6;
        static constexpr UINT source_status_result_message = WM_APP + 7;
        static constexpr UINT selection_timer_id = 1;
        static constexpr UINT hook_refresh_timer_id = 2;
        static constexpr UINT app_watchdog_timer_id = 3;
        static constexpr UINT selection_interval_ms = 25;
        static constexpr UINT selection_stale_after_ms = 500;
        static constexpr UINT selection_worker_stall_after_ms = 1000;
        static constexpr UINT selection_worker_max_restarts = 5;
        static constexpr UINT selection_worker_cooldown_ms = 30000;
        static constexpr UINT hook_refresh_interval_ms = 1000;
        static constexpr UINT app_watchdog_interval_ms =
            glance::contracts::process_watchdog_interval_ms;

        static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
        [[nodiscard]] bool create_message_window(HINSTANCE instance);
        [[nodiscard]] bool start_selection_worker();
        [[nodiscard]] bool start_selection_worker_generation();
        static void run_selection_worker(
            std::shared_ptr<SelectionWorkerContext> context,
            std::uint64_t generation);
        static void publish_selection(
            const std::shared_ptr<SelectionWorkerContext>& context,
            std::uint64_t generation,
            glance::contracts::SelectionSnapshot next,
            bool suppress_preview_update);
        static void publish_gallery_response(
            const std::shared_ptr<SelectionWorkerContext>& context,
            GalleryResponse response);
        void stop_selection_worker() noexcept;
        void apply_pending_selection();
        void apply_selection(
            glance::contracts::SelectionSnapshot next,
            bool suppress_preview_update = false);
        void apply_pending_gallery_commands();
        void apply_pending_gallery_responses();
        void apply_pending_source_status_responses();
        void monitor_selection_worker();
        [[nodiscard]] bool selection_worker_healthy() const noexcept;
        void handle_hook_action(HookAction action, std::uint64_t posted_at_ms);
        void handle_pipe_message(glance::contracts::MessageType type, std::uint32_t flags, std::string_view payload);
        void handle_connection_changed(bool connected);
        void recover_keyboard_hook(std::wstring_view reason);
        void supervise_app();
        void capture_app_process(DWORD process_id);
        void close_app_process() noexcept;
        void reset_app_health() noexcept;
        void terminate_unresponsive_app();
        [[nodiscard]] bool launch_app();
        [[nodiscard]] std::string make_open_payload(const glance::contracts::SelectionSnapshot& selection) const;
        [[nodiscard]] bool selection_changed(const glance::contracts::SelectionSnapshot& next) const;

        HWND window_{};
        unique_handle single_instance_mutex_;
        unique_handle elevated_status_mutex_;
        unique_handle shutdown_event_;
        unique_handle app_process_;
        unique_handle app_token_;
        DWORD app_process_id_{};
        bool elevated_{};
        std::atomic_bool shutting_down_{};
        std::uint32_t heartbeat_sequence_{};
        std::uint32_t pending_heartbeat_{};
        std::uint32_t missed_heartbeats_{};
        std::atomic_uint32_t last_heartbeat_ack_{};
        std::uint64_t app_connection_grace_until_ms_{};
        std::uint64_t last_app_launch_attempt_ms_{};
        InputDecisionState input_state_;
        glance::contracts::SelectionSnapshot selection_;
        std::shared_ptr<SelectionWorkerContext> selection_worker_context_;
        std::thread selection_worker_;
        std::uint64_t selection_worker_last_restart_ms_{};
        std::uint64_t selection_worker_cooldown_until_ms_{};
        std::uint32_t selection_worker_consecutive_stalls_{};
        std::uint64_t selection_generation_{};
        std::mutex gallery_payload_mutex_;
        std::vector<std::string> pending_gallery_payloads_;
        PipeServer pipe_server_;
        NetworkService network_service_;
        KeyboardHookService* keyboard_hook_{};
        std::atomic_uint64_t raw_input_count_{};
        std::uint64_t previous_raw_input_count_{};
        std::uint64_t previous_hook_event_count_{};
        std::atomic<glance::contracts::PreviewWindowState> preview_state_{
            glance::contracts::PreviewWindowState::hidden };
    };
}

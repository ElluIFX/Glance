#pragma once

#include "App.xaml.g.h"
#include "MainWindow.xaml.h"
#include "SettingsWindow.xaml.h"
#include "core_network_client.h"
#include "pipe_client.h"
#include "preview_file.h"
#include "tray_icon.h"

#include "glance/contracts/preview_state.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace winrt::Glance::App::implementation
{
    struct App : AppT<App>
    {
        App();
        ~App();

        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

    private:
        void ensure_core_started();
        void show_duplicate_instance_notice();
        void start_core_watchdog();
        void supervise_core();
        void refresh_core_process(DWORD process_id = 0);
        void close_core_process() noexcept;
        void reset_core_health() noexcept;
        void terminate_unresponsive_core();
        void create_active_window();
        void show_settings();
        void apply_appearance_preferences();
        void apply_text_preferences();
        void apply_footer_preferences();
        void apply_window_preferences();
        void request_automatic_update_check();
        void handle_automatic_update_result(
            std::optional<glance::contracts::UpdateCheckResult> result);
        void show_pending_update_prompt();
        winrt::fire_and_forget show_update_prompt(
            glance::contracts::UpdateCheckResult update);
        void exit_application();
        void handle_pipe_message(glance::contracts::MessageType type, std::uint32_t flags, std::string payload);
        void handle_connection_changed(bool connected);
        void handle_window_state(
            std::uint64_t instance_id,
            glance::contracts::PreviewWindowState state);
        void open_preview(std::string_view payload);
        void handle_preview_input(glance::contracts::PreviewInputAction action);
        void close_active_preview();

        HANDLE instance_mutex_{};
        HANDLE shutdown_event_{};
        std::atomic_bool shutting_down_{};
        Microsoft::UI::Xaml::Window duplicate_instance_window_{ nullptr };
        Microsoft::UI::Dispatching::DispatcherQueue dispatcher_{ nullptr };
        Microsoft::UI::Xaml::DispatcherTimer core_watchdog_timer_{ nullptr };
        glance::app::PipeClient pipe_client_;
        glance::app::CoreNetworkClient core_network_client_;
        HANDLE core_process_{};
        DWORD core_process_id_{};
        std::uint32_t heartbeat_sequence_{};
        std::uint32_t pending_heartbeat_{};
        std::uint32_t missed_heartbeats_{};
        std::atomic_uint32_t last_heartbeat_ack_{};
        std::uint64_t core_connection_grace_until_ms_{};
        std::uint64_t last_core_launch_attempt_ms_{};
        Glance::App::MainWindow active_window_{ nullptr };
        Glance::App::SettingsWindow settings_window_{ nullptr };
        std::vector<Glance::App::MainWindow> detached_windows_;
        std::unique_ptr<glance::app::TrayIcon> tray_icon_;
        std::uint64_t next_instance_id_{ 1 };
        bool automatic_update_check_in_flight_{};
        bool update_prompt_active_{};
        std::optional<glance::contracts::UpdateCheckResult> pending_update_;
        std::atomic<glance::contracts::PreviewWindowState> active_window_state_{
            glance::contracts::PreviewWindowState::hidden };
    };
}

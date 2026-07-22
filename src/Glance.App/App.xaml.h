#pragma once

#include "App.xaml.g.h"
#include "MainWindow.xaml.h"
#include "SettingsWindow.xaml.h"
#include "pipe_client.h"
#include "preview_file.h"
#include "tray_icon.h"

#include "glance/contracts/preview_state.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
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
        void exit_application();
        void handle_pipe_message(glance::contracts::MessageType type, std::uint32_t flags, std::string payload);
        void handle_connection_changed(bool connected);
        void handle_window_state(
            std::uint64_t instance_id,
            glance::contracts::PreviewWindowState state);
        void open_preview(std::string_view payload);
        void close_active_preview();

        HANDLE instance_mutex_{};
        HANDLE shutdown_event_{};
        std::atomic_bool shutting_down_{};
        Microsoft::UI::Xaml::Window duplicate_instance_window_{ nullptr };
        Microsoft::UI::Dispatching::DispatcherQueue dispatcher_{ nullptr };
        Microsoft::UI::Xaml::DispatcherTimer core_watchdog_timer_{ nullptr };
        glance::app::PipeClient pipe_client_;
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
        std::atomic<glance::contracts::PreviewWindowState> active_window_state_{
            glance::contracts::PreviewWindowState::hidden };
    };
}

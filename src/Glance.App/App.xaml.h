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
        void start_core_watchdog();
        void create_active_window();
        void show_settings();
        void apply_appearance_preferences();
        void apply_text_preferences();
        void exit_application();
        void handle_pipe_message(glance::contracts::MessageType type, std::uint32_t flags, std::string payload);
        void handle_connection_changed(bool connected);
        void handle_window_state(
            std::uint64_t instance_id,
            glance::contracts::PreviewWindowState state);
        void open_preview(std::string_view payload);
        void close_active_preview();

        HANDLE instance_mutex_{};
        std::atomic_bool shutting_down_{};
        Microsoft::UI::Dispatching::DispatcherQueue dispatcher_{ nullptr };
        Microsoft::UI::Xaml::DispatcherTimer core_watchdog_timer_{ nullptr };
        glance::app::PipeClient pipe_client_;
        Glance::App::MainWindow active_window_{ nullptr };
        Glance::App::SettingsWindow settings_window_{ nullptr };
        std::vector<Glance::App::MainWindow> detached_windows_;
        std::unique_ptr<glance::app::TrayIcon> tray_icon_;
        std::uint64_t next_instance_id_{ 1 };
    };
}

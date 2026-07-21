#include "pch.h"
#include "App.xaml.h"
#include "appearance_preferences.h"
#include "localization.h"
#include "MainWindow.xaml.h"
#include "SettingsWindow.xaml.h"
#include "glance/contracts/diagnostics.h"

#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <filesystem>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace
{
    std::filesystem::path executable_directory()
    {
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }
}

namespace winrt::Glance::App::implementation
{
    App::App()
        : pipe_client_(
              [this](auto type, auto flags, auto payload) {
                  handle_pipe_message(type, flags, std::move(payload));
              },
              [this](bool connected) { handle_connection_changed(connected); })
    {
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& event)
        {
            glance::contracts::log_event(
                L"WinUI unhandled exception: " + std::wstring(event.Message()));
        });
#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& event)
        {
            if (IsDebuggerPresent())
            {
                const auto message = event.Message();
                static_cast<void>(message);
                __debugbreak();
            }
        });
#endif
    }

    App::~App()
    {
        shutting_down_.store(true, std::memory_order_release);
        pipe_client_.stop();
        if (instance_mutex_ != nullptr)
        {
            CloseHandle(instance_mutex_);
            instance_mutex_ = nullptr;
        }
    }

    void App::OnLaunched(LaunchActivatedEventArgs const&)
    {
        glance::contracts::initialize_diagnostics(L"Glance.App");
        instance_mutex_ = CreateMutexW(nullptr, FALSE, L"Local\\Glance.App");
        if (instance_mutex_ == nullptr || GetLastError() == ERROR_ALREADY_EXISTS)
        {
            Microsoft::UI::Xaml::Application::Current().Exit();
            return;
        }

        dispatcher_ = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        const auto appearance = glance::app::load_appearance_preferences();
        glance::app::apply_ui_language(appearance.language);
        glance::app::apply_accent_resources(appearance);
        glance::contracts::log_event(L"Creating the initial preview window.");
        create_active_window();
        glance::contracts::log_event(L"Creating the notification area icon.");
        tray_icon_ = std::make_unique<glance::app::TrayIcon>();
        if (!tray_icon_->create(
                GetModuleHandleW(nullptr),
                [this] { show_settings(); },
                [this] { exit_application(); }))
        {
            tray_icon_.reset();
            show_settings();
        }
        ensure_core_started();
        glance::contracts::log_event(L"Starting the Core pipe client.");
        static_cast<void>(pipe_client_.start());
        start_core_watchdog();
    }

    void App::ensure_core_started()
    {
        HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\Glance.Core");
        if (mutex != nullptr)
        {
            CloseHandle(mutex);
            return;
        }

        const auto core_path = executable_directory() / L"Glance.Core.exe";
        if (!std::filesystem::exists(core_path))
        {
            glance::contracts::log_event(L"Glance.Core.exe was not found.");
            return;
        }

        const auto parameters = L"--parent-pid=" + std::to_wstring(GetCurrentProcessId());
        SHELLEXECUTEINFOW execute{ sizeof(SHELLEXECUTEINFOW) };
        execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        execute.lpVerb = L"runas";
        execute.lpFile = core_path.c_str();
        execute.lpParameters = parameters.c_str();
        execute.nShow = SW_HIDE;
        if (ShellExecuteExW(&execute))
        {
            glance::contracts::log_event(L"Core process launch requested with elevation.");
            if (execute.hProcess != nullptr)
            {
                CloseHandle(execute.hProcess);
            }
            return;
        }

        execute.lpVerb = nullptr;
        if (ShellExecuteExW(&execute) && execute.hProcess != nullptr)
        {
            glance::contracts::log_event(L"Core process launch requested without elevation.");
            CloseHandle(execute.hProcess);
        }
    }

    void App::start_core_watchdog()
    {
        if (core_watchdog_timer_ == nullptr)
        {
            core_watchdog_timer_ = DispatcherTimer();
            core_watchdog_timer_.Interval(std::chrono::seconds(3));
            core_watchdog_timer_.Tick([this](IInspectable const&, IInspectable const&) {
                if (!shutting_down_.load(std::memory_order_acquire))
                {
                    ensure_core_started();
                }
            });
        }
        core_watchdog_timer_.Start();
    }

    void App::create_active_window()
    {
        active_window_ = make<MainWindow>();
        auto implementation = get_self<implementation::MainWindow>(active_window_);
        implementation->InitializeSession(
            next_instance_id_++,
            [this](std::uint64_t instance_id, glance::contracts::PreviewWindowState state) {
                handle_window_state(instance_id, state);
            });
    }

    void App::show_settings()
    {
        if (settings_window_ == nullptr)
        {
            settings_window_ = make<SettingsWindow>();
            get_self<implementation::SettingsWindow>(settings_window_)->InitializeSession(
                [this] { exit_application(); },
                [this] { apply_appearance_preferences(); },
                [this] { apply_text_preferences(); });
            settings_window_.Closed([this](IInspectable const&, WindowEventArgs const&) {
                settings_window_ = nullptr;
            });
        }
        settings_window_.Activate();
    }

    void App::apply_appearance_preferences()
    {
        glance::app::apply_accent_resources(glance::app::load_appearance_preferences());
        if (active_window_ != nullptr)
        {
            const auto window = get_self<implementation::MainWindow>(active_window_);
            window->ApplyAppearancePreferences();
            window->ApplyLocalizedResources();
        }
        for (const auto& window : detached_windows_)
        {
            const auto implementation = get_self<implementation::MainWindow>(window);
            implementation->ApplyAppearancePreferences();
            implementation->ApplyLocalizedResources();
        }
        if (settings_window_ != nullptr)
        {
            const auto window = get_self<implementation::SettingsWindow>(settings_window_);
            window->ApplyAppearancePreferences();
            window->ApplyLocalizedResources();
        }
    }

    void App::apply_text_preferences()
    {
        if (active_window_ != nullptr)
        {
            get_self<implementation::MainWindow>(active_window_)->ApplyTextPreferences();
        }
        for (const auto& window : detached_windows_)
        {
            get_self<implementation::MainWindow>(window)->ApplyTextPreferences();
        }
    }

    void App::exit_application()
    {
        if (shutting_down_.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }
        if (core_watchdog_timer_ != nullptr)
        {
            core_watchdog_timer_.Stop();
        }
        const bool shutdown_sent = pipe_client_.send(glance::contracts::MessageType::shutdown);
        glance::contracts::log_event(
            shutdown_sent ? L"Core shutdown requested." : L"Core shutdown request could not be sent.");
        tray_icon_.reset();
        pipe_client_.stop();
        if (settings_window_ != nullptr)
        {
            settings_window_.Close();
            settings_window_ = nullptr;
        }
        for (const auto& window : detached_windows_)
        {
            window.Close();
        }
        detached_windows_.clear();
        if (active_window_ != nullptr)
        {
            active_window_.Close();
            active_window_ = nullptr;
        }
        Microsoft::UI::Xaml::Application::Current().Exit();
    }

    void App::handle_pipe_message(
        glance::contracts::MessageType type,
        std::uint32_t,
        std::string payload)
    {
        if (shutting_down_.load(std::memory_order_acquire))
        {
            return;
        }
        dispatcher_.TryEnqueue([this, type, payload = std::move(payload)]() mutable {
            if (shutting_down_.load(std::memory_order_acquire))
            {
                return;
            }
            if (type == glance::contracts::MessageType::open_active_preview)
            {
                open_preview(payload);
            }
            else if (type == glance::contracts::MessageType::close_active_preview)
            {
                close_active_preview();
            }
        });
    }

    void App::handle_connection_changed(bool connected)
    {
        if (shutting_down_.load(std::memory_order_acquire))
        {
            return;
        }
        glance::contracts::log_event(connected ? L"Core pipe connected." : L"Core pipe disconnected.");
        dispatcher_.TryEnqueue([this, connected] {
            if (shutting_down_.load(std::memory_order_acquire))
            {
                return;
            }
            if (connected)
            {
                if (core_watchdog_timer_ != nullptr)
                {
                    core_watchdog_timer_.Stop();
                }
                return;
            }
            close_active_preview();
            start_core_watchdog();
        });
    }

    void App::handle_window_state(
        std::uint64_t instance_id,
        glance::contracts::PreviewWindowState state)
    {
        if (shutting_down_.load(std::memory_order_acquire))
        {
            return;
        }
        const bool is_active_window = active_window_ != nullptr
            && get_self<implementation::MainWindow>(active_window_)->InstanceId() == instance_id;
        if (is_active_window)
        {
            static_cast<void>(pipe_client_.send(
                glance::contracts::MessageType::preview_state_changed,
                {},
                static_cast<std::uint32_t>(state)));
        }

        if (state == glance::contracts::PreviewWindowState::detached_pinned_topmost && is_active_window)
        {
            detached_windows_.push_back(active_window_);
            active_window_ = nullptr;
            create_active_window();
        }
        else if (state == glance::contracts::PreviewWindowState::closed)
        {
            // Closing a WinUI window synchronously raises WM_NCDESTROY. Keep its
            // final reference until the close callback and timer tick have unwound.
            static_cast<void>(dispatcher_.TryEnqueue([this, instance_id] {
                if (shutting_down_.load(std::memory_order_acquire))
                {
                    return;
                }
                const bool active = active_window_ != nullptr
                    && get_self<implementation::MainWindow>(active_window_)->InstanceId() == instance_id;
                if (active)
                {
                    active_window_ = nullptr;
                    create_active_window();
                    return;
                }
                std::erase_if(detached_windows_, [instance_id](const auto& window) {
                    return get_self<implementation::MainWindow>(window)->InstanceId() == instance_id;
                });
            }));
        }
    }

    void App::open_preview(std::string_view payload)
    {
        std::vector<glance::app::PreviewFile> files;
        std::uint32_t focused_index{};
        std::uint32_t source_kind{};
        std::uintptr_t source_window{};

        try
        {
            const auto root = winrt::Windows::Data::Json::JsonObject::Parse(to_hstring(payload));
            const auto json_files = root.GetNamedArray(L"files");
            files.reserve(json_files.Size());
            for (std::uint32_t index = 0; index < json_files.Size(); ++index)
            {
                const auto object = json_files.GetObjectAt(index);
                glance::app::PreviewFile file;
                file.display_name = object.GetNamedString(L"displayName").c_str();
                file.path = object.GetNamedString(L"path").c_str();
                file.parsing_name = object.GetNamedString(L"parsingName").c_str();
                file.size = std::stoull(object.GetNamedString(L"size").c_str());
                file.last_write_time = std::stoull(object.GetNamedString(L"lastWriteTime").c_str());
                file.attributes = static_cast<std::uint32_t>(object.GetNamedNumber(L"attributes"));
                file.is_filesystem = object.GetNamedBoolean(L"isFilesystem");
                file.is_cloud_placeholder = object.GetNamedBoolean(L"isCloudPlaceholder");
                files.push_back(std::move(file));
            }
            focused_index = static_cast<std::uint32_t>(root.GetNamedNumber(L"focusedIndex"));
            source_kind = static_cast<std::uint32_t>(root.GetNamedNumber(L"sourceKind"));
            source_window = static_cast<std::uintptr_t>(std::stoull(root.GetNamedString(L"sourceWindow").c_str()));
        }
        catch (const hresult_error&)
        {
            return;
        }

        if (active_window_ == nullptr)
        {
            glance::contracts::log_event(L"No active preview window; creating a replacement.");
            create_active_window();
        }
        glance::contracts::log_event(L"Dispatching the preview request to the active window.");
        get_self<implementation::MainWindow>(active_window_)->ShowPreview(
            std::move(files),
            focused_index,
            source_kind,
            reinterpret_cast<HWND>(source_window));
        glance::contracts::log_event(L"Preview request dispatch complete.");
    }

    void App::close_active_preview()
    {
        if (active_window_ != nullptr)
        {
            get_self<implementation::MainWindow>(active_window_)->HidePreview();
        }
    }
}

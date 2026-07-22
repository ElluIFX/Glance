#include "pch.h"
#include "App.xaml.h"
#include "appearance_preferences.h"
#include "localization.h"
#include "MainWindow.xaml.h"
#include "office_availability.h"
#include "pdf_render_client.h"
#include "resource.h"
#include "SettingsWindow.xaml.h"
#include "startup_registration.h"
#include "glance/contracts/diagnostics.h"

#include <shellapi.h>
#include <tlhelp32.h>
#include <microsoft.ui.xaml.window.h>

#include <algorithm>
#include <chrono>
#include <filesystem>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
namespace Controls = Microsoft::UI::Xaml::Controls;
namespace Media = Microsoft::UI::Xaml::Media;

namespace
{
    std::filesystem::path executable_directory()
    {
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

    std::vector<std::wstring> command_line_arguments()
    {
        int count{};
        LPWSTR* raw = CommandLineToArgvW(GetCommandLineW(), &count);
        if (raw == nullptr)
        {
            return {};
        }
        std::vector<std::wstring> result;
        result.reserve(count > 1 ? static_cast<std::size_t>(count - 1) : 0U);
        for (int index = 1; index < count; ++index)
        {
            result.emplace_back(raw[index]);
        }
        LocalFree(raw);
        return result;
    }

    std::wstring executable_path()
    {
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size())
        {
            return {};
        }
        path.resize(length);
        return path;
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

    HANDLE find_process_by_path(const std::filesystem::path& expected_path, DWORD& process_id) noexcept
    {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return nullptr;
        }

        DWORD current_session{};
        static_cast<void>(ProcessIdToSessionId(GetCurrentProcessId(), &current_session));
        PROCESSENTRY32W entry{ sizeof(PROCESSENTRY32W) };
        HANDLE result{};
        if (Process32FirstW(snapshot, &entry))
        {
            do
            {
                DWORD session{};
                if (!ProcessIdToSessionId(entry.th32ProcessID, &session) || session != current_session)
                {
                    continue;
                }
                HANDLE process = open_supervised_process(entry.th32ProcessID);
                if (process == nullptr)
                {
                    continue;
                }
                std::wstring path(32768, L'\0');
                DWORD length = static_cast<DWORD>(path.size());
                if (QueryFullProcessImageNameW(process, 0, path.data(), &length))
                {
                    path.resize(length);
                    if (paths_equal(path, expected_path.wstring()))
                    {
                        result = process;
                        process_id = entry.th32ProcessID;
                        break;
                    }
                }
                CloseHandle(process);
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return result;
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
        close_core_process();
        if (shutdown_event_ != nullptr)
        {
            CloseHandle(shutdown_event_);
            shutdown_event_ = nullptr;
        }
        if (instance_mutex_ != nullptr)
        {
            CloseHandle(instance_mutex_);
            instance_mutex_ = nullptr;
        }
    }

    void App::OnLaunched(LaunchActivatedEventArgs const&)
    {
#if defined _DEBUG
        std::wstring debug_preview_path;
#endif
        for (const auto& argument : command_line_arguments())
        {
            if (argument == L"--set-startup=enabled")
            {
                ExitProcess(glance::app::set_launch_at_sign_in(true) ? 0 : 1);
            }
            if (argument == L"--set-startup=disabled")
            {
                ExitProcess(glance::app::set_launch_at_sign_in(false) ? 0 : 1);
            }
            if (argument == L"--cleanup-startup")
            {
                const auto path = executable_path();
                ExitProcess(!path.empty() && glance::app::cleanup_launch_at_sign_in(path) ? 0 : 1);
            }
#if defined _DEBUG
            constexpr std::wstring_view debug_preview_prefix = L"--debug-preview=";
            if (argument.starts_with(debug_preview_prefix))
            {
                debug_preview_path = argument.substr(debug_preview_prefix.size());
            }
#endif
        }

        glance::contracts::initialize_diagnostics(L"Glance.App");
        instance_mutex_ = CreateMutexW(nullptr, FALSE, L"Local\\Glance.App");
        if (instance_mutex_ == nullptr)
        {
            Microsoft::UI::Xaml::Application::Current().Exit();
            return;
        }
        const bool duplicate_instance = GetLastError() == ERROR_ALREADY_EXISTS;
        dispatcher_ = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        const auto appearance = glance::app::load_appearance_preferences();
        glance::app::apply_ui_language(appearance.language);
        glance::app::apply_accent_resources(appearance);
        if (duplicate_instance)
        {
            show_duplicate_instance_notice();
            return;
        }
        shutdown_event_ = CreateEventW(nullptr, TRUE, FALSE, L"Local\\Glance.Shutdown");
        if (shutdown_event_ != nullptr)
        {
            ResetEvent(shutdown_event_);
        }

        glance::app::initialize_office_availability();
        glance::app::prewarm_pdf_render_client();
        glance::contracts::log_event(L"Creating the initial preview window.");
        create_active_window();
#if defined _DEBUG
        if (!debug_preview_path.empty())
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (GetFileAttributesExW(
                    debug_preview_path.c_str(),
                    GetFileExInfoStandard,
                    &data))
            {
                glance::app::PreviewFile file;
                file.path = debug_preview_path;
                file.parsing_name = debug_preview_path;
                file.display_name = std::filesystem::path(debug_preview_path).filename().wstring();
                file.size =
                    (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32U) |
                    data.nFileSizeLow;
                file.creation_time =
                    (static_cast<std::uint64_t>(data.ftCreationTime.dwHighDateTime) << 32U) |
                    data.ftCreationTime.dwLowDateTime;
                file.last_write_time =
                    (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32U) |
                    data.ftLastWriteTime.dwLowDateTime;
                file.attributes = data.dwFileAttributes;
                file.is_filesystem = true;
                get_self<implementation::MainWindow>(active_window_)->ShowPreview(
                    { std::move(file) },
                    0,
                    0,
                    nullptr);
            }
        }
#endif
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

    void App::show_duplicate_instance_notice()
    {
        try
        {
            duplicate_instance_window_ = Window();
            duplicate_instance_window_.Title(L"Glance");
            const auto root = Controls::Grid();
            root.RequestedTheme(glance::app::element_theme(
                glance::app::load_appearance_preferences().theme));
            root.Background(Application::Current().Resources()
                .Lookup(box_value(L"ApplicationPageBackgroundThemeBrush"))
                .as<Media::Brush>());
            const auto content = Controls::StackPanel();
            content.Margin(Thickness{ 24, 24, 24, 20 });
            content.Spacing(20);
            content.VerticalAlignment(VerticalAlignment::Center);

            const auto message = Controls::TextBlock();
            message.Text(glance::app::localize(L"DuplicateInstanceMessage"));
            message.FontSize(15);
            message.TextWrapping(TextWrapping::NoWrap);
            content.Children().Append(message);

            const auto confirm = Controls::Button();
            confirm.Content(box_value(glance::app::localize(L"OK")));
            confirm.HorizontalAlignment(HorizontalAlignment::Right);
            confirm.Click([this](IInspectable const&, RoutedEventArgs const&) {
                if (duplicate_instance_window_ != nullptr)
                {
                    duplicate_instance_window_.Close();
                }
            });
            content.Children().Append(confirm);
            root.Children().Append(content);
            duplicate_instance_window_.Content(root);

            HWND window{};
            check_hresult(duplicate_instance_window_.try_as<::IWindowNative>()->get_WindowHandle(&window));
            if (const HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_GLANCE_APP)))
            {
                SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
                SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
            }
            if (const auto presenter = duplicate_instance_window_.AppWindow().Presenter()
                    .try_as<Microsoft::UI::Windowing::OverlappedPresenter>())
            {
                presenter.IsResizable(false);
                presenter.IsMinimizable(false);
                presenter.IsMaximizable(false);
            }

            constexpr int logical_width = 480;
            constexpr int logical_height = 160;
            const UINT dpi = GetDpiForWindow(window);
            const int width = MulDiv(logical_width, dpi, 96);
            const int height = MulDiv(logical_height, dpi, 96);
            MONITORINFO monitor_info{ sizeof(monitor_info) };
            GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitor_info);
            const int x = monitor_info.rcWork.left +
                ((monitor_info.rcWork.right - monitor_info.rcWork.left) - width) / 2;
            const int y = monitor_info.rcWork.top +
                ((monitor_info.rcWork.bottom - monitor_info.rcWork.top) - height) / 2;
            SetWindowPos(window, nullptr, x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER);

            duplicate_instance_window_.Closed([this](IInspectable const&, WindowEventArgs const&) {
                duplicate_instance_window_ = nullptr;
                Microsoft::UI::Xaml::Application::Current().Exit();
            });
            duplicate_instance_window_.Activate();
            static_cast<void>(confirm.Focus(FocusState::Programmatic));
        }
        catch (const hresult_error& error)
        {
            glance::contracts::log_event(
                L"Duplicate instance notice failed: " + std::wstring(error.message()));
            if (duplicate_instance_window_ != nullptr)
            {
                duplicate_instance_window_.Close();
            }
            else
            {
                Microsoft::UI::Xaml::Application::Current().Exit();
            }
        }
    }

    void App::ensure_core_started()
    {
        refresh_core_process();
        if (core_process_ != nullptr && WaitForSingleObject(core_process_, 0) == WAIT_TIMEOUT)
        {
            return;
        }
        close_core_process();

        HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\Glance.Core");
        if (mutex != nullptr)
        {
            CloseHandle(mutex);
            return;
        }

        const auto now = GetTickCount64();
        if (last_core_launch_attempt_ms_ != 0 && now - last_core_launch_attempt_ms_ < 2000)
        {
            return;
        }
        last_core_launch_attempt_ms_ = now;

        const auto core_path = executable_directory() / L"Glance.Core.exe";
        if (!std::filesystem::exists(core_path))
        {
            glance::contracts::log_event(L"Glance.Core.exe was not found.");
            return;
        }

        const auto parameters = L"--app-pid=" + std::to_wstring(GetCurrentProcessId());
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
                core_process_ = execute.hProcess;
                core_process_id_ = GetProcessId(execute.hProcess);
                core_connection_grace_until_ms_ = now + 5000;
            }
            return;
        }

        execute.lpVerb = nullptr;
        if (ShellExecuteExW(&execute) && execute.hProcess != nullptr)
        {
            glance::contracts::log_event(L"Core process launch requested without elevation.");
            core_process_ = execute.hProcess;
            core_process_id_ = GetProcessId(execute.hProcess);
            core_connection_grace_until_ms_ = now + 5000;
        }
    }

    void App::start_core_watchdog()
    {
        if (core_watchdog_timer_ == nullptr)
        {
            core_watchdog_timer_ = DispatcherTimer();
            core_watchdog_timer_.Interval(std::chrono::milliseconds(
                glance::contracts::process_watchdog_interval_ms));
            core_watchdog_timer_.Tick([this](IInspectable const&, IInspectable const&) {
                if (!shutting_down_.load(std::memory_order_acquire))
                {
                    supervise_core();
                }
            });
        }
        core_watchdog_timer_.Start();
    }

    void App::supervise_core()
    {
        refresh_core_process();
        if (core_process_ == nullptr)
        {
            reset_core_health();
            ensure_core_started();
            return;
        }
        if (WaitForSingleObject(core_process_, 0) == WAIT_OBJECT_0)
        {
            glance::contracts::log_event(L"Core process exited unexpectedly; restarting it.");
            close_core_process();
            reset_core_health();
            ensure_core_started();
            return;
        }

        const auto now = GetTickCount64();
        if (!pipe_client_.connected())
        {
            pending_heartbeat_ = 0;
            if (now < core_connection_grace_until_ms_)
            {
                return;
            }
            if (++missed_heartbeats_ >= glance::contracts::process_watchdog_failure_limit)
            {
                terminate_unresponsive_core();
            }
            return;
        }

        if (pending_heartbeat_ != 0)
        {
            if (last_heartbeat_ack_.load(std::memory_order_acquire) == pending_heartbeat_)
            {
                missed_heartbeats_ = 0;
            }
            else if (++missed_heartbeats_ >= glance::contracts::process_watchdog_failure_limit)
            {
                terminate_unresponsive_core();
                return;
            }
        }

        const auto sequence = ++heartbeat_sequence_;
        if (pipe_client_.send(glance::contracts::MessageType::heartbeat, {}, sequence))
        {
            pending_heartbeat_ = sequence;
        }
    }

    void App::refresh_core_process(DWORD process_id)
    {
        if (process_id == 0)
        {
            process_id = pipe_client_.connected() ? pipe_client_.peer_process_id() : 0;
        }
        if (process_id == 0 && core_process_ != nullptr &&
            WaitForSingleObject(core_process_, 0) == WAIT_TIMEOUT)
        {
            return;
        }
        if (process_id != 0 && process_id == core_process_id_ && core_process_ != nullptr)
        {
            return;
        }

        HANDLE process{};
        if (process_id != 0)
        {
            process = open_supervised_process(process_id);
        }
        if (process == nullptr)
        {
            process_id = 0;
            process = find_process_by_path(executable_directory() / L"Glance.Core.exe", process_id);
        }
        if (process == nullptr)
        {
            return;
        }

        close_core_process();
        core_process_ = process;
        core_process_id_ = process_id;
        core_connection_grace_until_ms_ = GetTickCount64() + 5000;
    }

    void App::close_core_process() noexcept
    {
        if (core_process_ != nullptr)
        {
            CloseHandle(core_process_);
            core_process_ = nullptr;
        }
        core_process_id_ = 0;
    }

    void App::reset_core_health() noexcept
    {
        pending_heartbeat_ = 0;
        missed_heartbeats_ = 0;
        last_heartbeat_ack_.store(0, std::memory_order_release);
    }

    void App::terminate_unresponsive_core()
    {
        glance::contracts::log_event(
            L"Core health check failed five consecutive times; terminating it for recovery.");
        static_cast<void>(pipe_client_.send(glance::contracts::MessageType::terminate_unresponsive));
        if (core_process_ != nullptr)
        {
            static_cast<void>(TerminateProcess(core_process_, ERROR_PROCESS_ABORTED));
        }
        reset_core_health();
        core_connection_grace_until_ms_ =
            GetTickCount64() + glance::contracts::process_watchdog_interval_ms;
    }

    void App::create_active_window()
    {
        active_window_state_.store(
            glance::contracts::PreviewWindowState::hidden,
            std::memory_order_release);
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
                [this] { apply_text_preferences(); },
                [this] { apply_footer_preferences(); },
                [this] { apply_window_preferences(); });
            settings_window_.Closed([this](IInspectable const&, WindowEventArgs const&) {
                settings_window_ = nullptr;
            });
        }
        get_self<implementation::SettingsWindow>(settings_window_)->ShowAndActivate();
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

    void App::apply_footer_preferences()
    {
        if (active_window_ != nullptr)
        {
            get_self<implementation::MainWindow>(active_window_)->ApplyFooterPreferences();
        }
        for (const auto& window : detached_windows_)
        {
            get_self<implementation::MainWindow>(window)->ApplyFooterPreferences();
        }
    }

    void App::apply_window_preferences()
    {
        if (active_window_ != nullptr)
        {
            get_self<implementation::MainWindow>(active_window_)->ApplyWindowPreferences();
        }
        for (const auto& window : detached_windows_)
        {
            get_self<implementation::MainWindow>(window)->ApplyWindowPreferences();
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
        if (shutdown_event_ != nullptr)
        {
            SetEvent(shutdown_event_);
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
        std::uint32_t flags,
        std::string payload)
    {
        if (type == glance::contracts::MessageType::terminate_unresponsive)
        {
            glance::contracts::log_event(L"Core requested emergency UI termination.");
            TerminateProcess(GetCurrentProcess(), ERROR_PROCESS_ABORTED);
            return;
        }
        if (type == glance::contracts::MessageType::heartbeat_ack)
        {
            last_heartbeat_ack_.store(flags, std::memory_order_release);
            return;
        }
        if (shutting_down_.load(std::memory_order_acquire))
        {
            return;
        }
        dispatcher_.TryEnqueue([this, type, flags, payload = std::move(payload)]() mutable {
            if (shutting_down_.load(std::memory_order_acquire))
            {
                return;
            }
            if (type == glance::contracts::MessageType::heartbeat)
            {
                static_cast<void>(pipe_client_.send(
                    glance::contracts::MessageType::heartbeat_ack,
                    {},
                    flags));
            }
            else if (type == glance::contracts::MessageType::hello_ack)
            {
                static_cast<void>(pipe_client_.send(
                    glance::contracts::MessageType::preview_state_changed,
                    {},
                    static_cast<std::uint32_t>(active_window_state_.load(std::memory_order_acquire))));
            }
            else if (type == glance::contracts::MessageType::open_active_preview)
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
                refresh_core_process(pipe_client_.peer_process_id());
                reset_core_health();
                core_connection_grace_until_ms_ = 0;
                return;
            }
            reset_core_health();
            core_connection_grace_until_ms_ = GetTickCount64();
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
            active_window_state_.store(state, std::memory_order_release);
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
                file.creation_time = std::stoull(object.GetNamedString(L"creationTime").c_str());
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

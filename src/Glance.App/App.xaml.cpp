#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "SettingsWindow.xaml.h"

#include <shellapi.h>

#include <algorithm>
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
        instance_mutex_ = CreateMutexW(nullptr, FALSE, L"Local\\Glance.App");
        if (instance_mutex_ == nullptr || GetLastError() == ERROR_ALREADY_EXISTS)
        {
            Microsoft::UI::Xaml::Application::Current().Exit();
            return;
        }

        dispatcher_ = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        create_active_window();
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
        static_cast<void>(pipe_client_.start());
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
            return;
        }

        SHELLEXECUTEINFOW execute{ sizeof(SHELLEXECUTEINFOW) };
        execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        execute.lpVerb = L"runas";
        execute.lpFile = core_path.c_str();
        execute.nShow = SW_HIDE;
        if (ShellExecuteExW(&execute))
        {
            if (execute.hProcess != nullptr)
            {
                CloseHandle(execute.hProcess);
            }
            return;
        }

        execute.lpVerb = nullptr;
        if (ShellExecuteExW(&execute) && execute.hProcess != nullptr)
        {
            CloseHandle(execute.hProcess);
        }
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
                [this] { exit_application(); });
            settings_window_.Closed([this](IInspectable const&, WindowEventArgs const&) {
                settings_window_ = nullptr;
            });
        }
        settings_window_.Activate();
    }

    void App::exit_application()
    {
        if (shutting_down_.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }
        static_cast<void>(pipe_client_.send(glance::contracts::MessageType::shutdown));
        tray_icon_.reset();
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
        if (!connected)
        {
            dispatcher_.TryEnqueue([this] {
                if (!shutting_down_.load(std::memory_order_acquire))
                {
                    close_active_preview();
                }
            });
        }
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
            if (is_active_window)
            {
                active_window_ = nullptr;
                create_active_window();
                return;
            }
            std::erase_if(detached_windows_, [instance_id](const auto& window) {
                return get_self<implementation::MainWindow>(window)->InstanceId() == instance_id;
            });
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
            create_active_window();
        }
        get_self<implementation::MainWindow>(active_window_)->ShowPreview(
            std::move(files),
            focused_index,
            source_kind,
            reinterpret_cast<HWND>(source_window));
    }

    void App::close_active_preview()
    {
        if (active_window_ != nullptr)
        {
            get_self<implementation::MainWindow>(active_window_)->HidePreview();
        }
    }
}

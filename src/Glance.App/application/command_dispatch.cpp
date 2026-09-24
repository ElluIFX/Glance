#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "SettingsWindow.xaml.h"
#include "settings_commands.h"
#include "localization.h"
#include "appearance_preferences.h"
#include "../version.h"
#include "glance/contracts/cli_protocol.h"
#include "glance/contracts/cli_input.h"
#include <future>
#include <set>

using namespace winrt;
using namespace Windows::Data::Json;
using glance::cli::Error;

namespace
{
    JsonObject response(std::wstring_view command, JsonObject const& data, const Error* error = nullptr)
    {
        JsonObject result;
        result.SetNamedValue(L"schema_version", JsonValue::CreateNumberValue(1));
        result.SetNamedValue(L"ok", JsonValue::CreateBooleanValue(error == nullptr));
        result.SetNamedValue(L"command", JsonValue::CreateStringValue(command));
        result.SetNamedValue(L"data", error ? JsonValue::CreateNullValue() : data.as<IJsonValue>());
        if (error)
        {
            JsonObject detail;
            detail.SetNamedValue(L"code", JsonValue::CreateNumberValue(error->code));
            detail.SetNamedValue(L"name", JsonValue::CreateStringValue(to_hstring(error->name)));
            detail.SetNamedValue(L"message", JsonValue::CreateStringValue(to_hstring(error->what())));
            result.SetNamedValue(L"error", detail);
        }
        else result.SetNamedValue(L"error", JsonValue::CreateNullValue());
        return result;
    }
}

namespace winrt::Glance::App::implementation
{
    std::string App::handle_cli_request(std::string payload, HANDLE cancelled, HANDLE connection)
    {
        hstring command;
        try
        {
            auto request = JsonObject::Parse(to_hstring(payload));
            command = request.GetNamedString(L"command");
            const auto duration = request.GetNamedNumber(L"timeout_ms", 10000);
            if (!std::isfinite(duration) || duration < 1 || duration > 86400000) throw Error(2, "invalid_timeout", "Invalid timeout");
            const auto deadline = GetTickCount64() + static_cast<ULONGLONG>(duration);
            const auto interrupted = [&] {
                return WaitForSingleObject(cancelled, 0) == WAIT_OBJECT_0 ||
                    !PeekNamedPipe(connection, nullptr, 0, nullptr, nullptr, nullptr) || GetTickCount64() >= deadline;
            };
            const std::set<std::wstring> fields{ L"command", L"timeout_ms", L"paths", L"id", L"stdin_file", L"generation", L"size", L"position", L"center_offset", L"monitor", L"close_after", L"topmost", L"pin", L"enabled", L"key", L"value" };
            for (const auto& item : request) if (!fields.contains(std::wstring(item.Key()))) throw Error(2, "unknown_field", "Unknown request field");
            if (request.HasKey(L"position") && request.HasKey(L"center_offset")) throw Error(2, "position_conflict", "Position modes are mutually exclusive");
            std::vector<glance::app::PreviewFile> files;
            if (command == L"preview" || command == L"window.set")
            {
                auto paths = request.GetNamedArray(L"paths");
                if (!paths.Size() || paths.Size() > 4096) throw Error(2, "invalid_paths", "Expected 1 to 4096 paths");
                for (const auto& value : paths)
                {
                    if (interrupted()) throw Error(5, "command_timeout", "Path validation timed out");
                    glance::app::PreviewFile file;
                    file.path = value.GetString();
                    if (file.path.find(L'\0') != std::wstring::npos || !std::filesystem::path(file.path).is_absolute()) throw Error(2, "invalid_path", "Expected absolute filesystem path");
                    if (request.GetNamedBoolean(L"stdin_file", false))
                    {
                        const auto source = std::filesystem::path(file.path).lexically_normal();
                        ULONG client_pid{};
                        if (command != L"preview" || paths.Size() != 1 ||
                            !GetNamedPipeClientProcessId(connection, &client_pid) ||
                            source.parent_path().parent_path() != glance::cli::input_root() ||
                            !source.parent_path().filename().wstring().starts_with(std::to_wstring(client_pid) + L"-"))
                            throw Error(2, "invalid_input_file", "Invalid temporary input path");
                        auto lease = glance::cli::create_input_lease();
                        const auto target = lease->directory / source.filename();
                        if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
                            throw Error(7, "input_transfer", "Cannot take ownership of temporary input");
                        file.path = target.wstring();
                        file.materialized_lease = std::move(lease);
                    }
                    WIN32_FILE_ATTRIBUTE_DATA info{};
                    if (!GetFileAttributesExW(file.path.c_str(), GetFileExInfoStandard, &info))
                        throw Error(GetLastError() == ERROR_ACCESS_DENIED ? 7 : 3, "path_unavailable", "Cannot access preview path");
                    file.parsing_name = file.path;
                    file.display_name = std::filesystem::path(file.path).filename().wstring();
                    file.is_filesystem = true;
                    file.attributes = info.dwFileAttributes;
                    file.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
                    file.creation_time = (static_cast<std::uint64_t>(info.ftCreationTime.dwHighDateTime) << 32) | info.ftCreationTime.dwLowDateTime;
                    file.last_write_time = (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
                    files.push_back(std::move(file));
                }
            }
            const auto on_ui = [&](JsonObject message, std::vector<glance::app::PreviewFile> preview_files) {
                auto completion = std::make_shared<std::promise<JsonObject>>();
                auto future = completion->get_future();
                auto abandoned = std::make_shared<std::atomic_bool>(false);
                if (!dispatcher_.TryEnqueue([this, completion, abandoned, deadline, message,
                    preview_files = std::move(preview_files)]() mutable {
                    try
                    {
                        if (abandoned->load() || GetTickCount64() >= deadline || shutting_down_.load()) throw Error(10, "cancelled", "Command cancelled before execution");
                        completion->set_value(execute_cli_command(message, std::move(preview_files)));
                    }
                    catch (...) { completion->set_exception(std::current_exception()); }
                })) throw Error(4, "app_exiting", "App is exiting");
                while (future.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready)
                {
                    if (interrupted())
                    { abandoned->store(true); throw Error(5, "command_timeout", "Command timed out"); }
                }
                return future.get();
            };
            JsonObject data;
            if (command == L"check-update")
            {
                while (!pipe_client_.connected() && !interrupted())
                    WaitForSingleObject(cancelled, 25);
                if (interrupted()) throw Error(5, "command_timeout", "Update check timed out");
                auto update = core_network_client_.check_for_updates(GLANCE_VERSION_WSTRING, interrupted);
                if (interrupted()) throw Error(5, "command_timeout", "Update check timed out");
                if (update.status != glance::contracts::UpdateCheckStatus::up_to_date &&
                    update.status != glance::contracts::UpdateCheckStatus::update_available)
                    throw Error(11, "update_check_failed", "Update check unavailable or rate limited");
                data.SetNamedValue(L"current_version", JsonValue::CreateStringValue(GLANCE_VERSION_WSTRING));
                data.SetNamedValue(L"latest_version", JsonValue::CreateStringValue(update.latest_version));
                data.SetNamedValue(L"update_available", JsonValue::CreateBooleanValue(update.status == glance::contracts::UpdateCheckStatus::update_available));
                data.SetNamedValue(L"release_url", JsonValue::CreateStringValue(update.release_url));
                data.SetNamedValue(L"download_url", JsonValue::CreateStringValue(update.installer.download_url));
            }
            else data = on_ui(request, std::move(files));
            return to_string(response(command, data).Stringify());
        }
        catch (const Error& error) { return to_string(response(command, JsonObject(), &error).Stringify()); }
        catch (const hresult_error& error)
        {
            Error detail(2, "invalid_request", to_string(error.message()));
            return to_string(response(command, JsonObject(), &detail).Stringify());
        }
        catch (const std::exception& error)
        {
            Error detail(1, "internal_error", error.what());
            return to_string(response(command, JsonObject(), &detail).Stringify());
        }
    }

    JsonObject App::execute_cli_command(JsonObject const& request, std::vector<glance::app::PreviewFile> files)
    {
        const auto command = request.GetNamedString(L"command");
        JsonObject result;
        if (std::wstring_view(command).starts_with(L"settings."))
        {
            result = glance::app::execute_settings_command(command, request);
            if (command == L"settings.set" || command == L"settings.reset")
            {
                const std::wstring key(request.GetNamedString(L"key"));
                if (key.starts_with(L"Appearance/"))
                {
                    glance::app::apply_ui_language(glance::app::load_appearance_preferences().language);
                    apply_appearance_preferences();
                }
                if (key.starts_with(L"TextPreview/")) apply_text_preferences();
                if (key.starts_with(L"Window/")) apply_window_preferences();
                if (key.starts_with(L"Footer/")) apply_footer_preferences();
                if (key.starts_with(L"Update/"))
                {
                    pending_update_.reset();
                    core_network_client_.forget_automatic_update_requests();
                }
                if (key.starts_with(L"Components/"))
                {
                    const auto end = key.find(L'/', 11);
                    auto component = key.substr(11, end - 11);
                    if (active_window_) get_self<MainWindow>(active_window_)->ApplyComponentSettings(component);
                    for (auto const& window : detached_windows_) get_self<MainWindow>(window)->ApplyComponentSettings(component);
                }
                if (settings_window_) get_self<SettingsWindow>(settings_window_)->ReloadPreferences();
            }
            return result;
        }
        if (command == L"status")
        {
            result.SetNamedValue(L"version", JsonValue::CreateStringValue(GLANCE_VERSION_WSTRING));
            result.SetNamedValue(L"protocol_version", JsonValue::CreateNumberValue(glance::cli::version));
            result.SetNamedValue(L"session_id", JsonValue::CreateStringValue(cli_session_id_));
            result.SetNamedValue(L"core_connected", JsonValue::CreateBooleanValue(pipe_client_.connected()));
            result.SetNamedValue(L"process_id", JsonValue::CreateNumberValue(GetCurrentProcessId()));
            result.SetNamedValue(L"core_process_id", JsonValue::CreateNumberValue(core_process_id_));
            JsonArray monitors;
            EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM context) -> BOOL {
                auto& array = *reinterpret_cast<JsonArray*>(context);
                MONITORINFO info{ sizeof(info) };
                if (GetMonitorInfoW(monitor, &info))
                {
                    JsonObject entry;
                    entry.SetNamedValue(L"index", JsonValue::CreateNumberValue(array.Size()));
                    entry.SetNamedValue(L"x", JsonValue::CreateNumberValue(info.rcWork.left));
                    entry.SetNamedValue(L"y", JsonValue::CreateNumberValue(info.rcWork.top));
                    entry.SetNamedValue(L"width", JsonValue::CreateNumberValue(info.rcWork.right - info.rcWork.left));
                    entry.SetNamedValue(L"height", JsonValue::CreateNumberValue(info.rcWork.bottom - info.rcWork.top));
                    array.Append(entry);
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&monitors));
            result.SetNamedValue(L"monitors", monitors);
            return result;
        }
        if (command == L"quit")
        {
            result.SetNamedValue(L"process_id", JsonValue::CreateNumberValue(GetCurrentProcessId()));
            result.SetNamedValue(L"core_process_id", JsonValue::CreateNumberValue(core_process_id_));
            return result;
        }
        if (command == L"windows")
        {
            JsonArray windows;
            for (const auto& window : detached_windows_)
            {
                auto item = get_self<MainWindow>(window)->CliSnapshot();
                if (item.GetNamedString(L"state") == L"closed") continue;
                windows.Append(item);
            }
            if (active_window_ && get_self<MainWindow>(active_window_)->CliLoadState() != L"closed")
            {
                auto item = get_self<MainWindow>(active_window_)->CliSnapshot();
                windows.Append(item);
            }
            result.SetNamedValue(L"windows", windows);
            return result;
        }
        auto window = active_window_;
        if (command != L"preview")
        {
            const auto id_text = request.GetNamedString(L"id", cli_last_window_id_);
            window = nullptr;
            for (const auto& item : detached_windows_)
                if (get_self<MainWindow>(item)->CliId() == id_text) window = item;
            if (active_window_ && get_self<MainWindow>(active_window_)->CliId() == id_text) window = active_window_;
            if (id_text == L"main") window = active_window_;
        }
        if (!window) throw Error(3, "window_not_found", "Window ID not found");
        auto implementation = get_self<MainWindow>(window);
        if (command != L"preview" && implementation->CliLoadState() == L"closed")
            throw Error(3, "window_not_found", "Window is closed");
        if (request.HasKey(L"generation") && implementation->CliSnapshot().GetNamedString(L"generation") != request.GetNamedString(L"generation"))
            throw Error(10, "preview_replaced", "Preview content changed before command execution");
        if (command == L"preview")
        {
            implementation->CliConfigure(request, true);
            implementation->ShowPreview(std::move(files), 0, 0, nullptr);
            implementation->CliConfigure(request);
            cli_last_window_id_ = implementation->CliId();
        }
        else if (command == L"window.set")
        {
            implementation->ShowPreview(std::move(files), 0, 0, nullptr, {}, 0, true);
        }
        else if (command == L"window.close")
        {
            result = implementation->CliSnapshot();
            if (window == active_window_) implementation->HidePreview(); else implementation->CloseForReplacement();
            result.SetNamedValue(L"state", JsonValue::CreateStringValue(L"closed"));
            return result;
        }
        else if (command == L"window.pin")
        {
            result = implementation->CliSnapshot();
            implementation->CliPin(request.GetNamedBoolean(L"enabled"));
            if (result.GetNamedBoolean(L"pinned") && !request.GetNamedBoolean(L"enabled"))
            {
                result.SetNamedValue(L"state", JsonValue::CreateStringValue(L"closed"));
            }
            else result = implementation->CliSnapshot();
            return result;
        }
        else if (command == L"window.topmost") implementation->CliTopmost(request.GetNamedBoolean(L"enabled"));
        else if (command == L"window.activate") implementation->CliActivate();
        else if (command == L"window.fullwindow") implementation->CliFullwindow(request.GetNamedBoolean(L"enabled"));
        else if (command == L"window.move" || command == L"window.resize") implementation->CliConfigure(request);
        else if (command != L"window.get")
        {
            implementation->CliExecuteControl(request);
            result = implementation->CliSnapshot();
            return result;
        }
        result = implementation->CliSnapshot();
        return result;
    }
}

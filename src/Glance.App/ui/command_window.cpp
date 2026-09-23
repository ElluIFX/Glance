#include "pch.h"
#include "MainWindow.xaml.h"
#include "glance/contracts/cli_protocol.h"
#include <cmath>

using namespace winrt;
using namespace Windows::Data::Json;
using Microsoft::UI::Xaml::Visibility;

namespace winrt::Glance::App::implementation
{
    std::wstring MainWindow::CliLoadState()
    {
        if (!visible_) return L"hidden";
        if (native_media_controls_pending_ || gallery_pending_navigation_steps_) return L"loading";
        if (ErrorText().Visibility() == Visibility::Visible && !ErrorText().Text().empty()) return L"failed";
        using Kind = glance::app::PreviewKind;
        bool ready = false;
        switch (current_kind_)
        {
        case Kind::generic: ready = true; break;
        case Kind::component: break;
        case Kind::image: ready = image_pixel_width_ > 0 && ImagePreview().Source() != nullptr; break;
        case Kind::document: ready = PdfPageImage().Source() != nullptr && pdf_foreground_render_requests_.load() == 0; break;
        case Kind::native_document: ready = native_preview_ready_; break;
        case Kind::archive: ready = archive_render_state_ != nullptr; break;
        case Kind::text: ready = !text_loading_; break;
        case Kind::markdown:
        case Kind::web: ready = !text_loading_ && (!markdown_preview_ || web_content_ready_); break;
        case Kind::media:
            ready = native_media_active_ ? native_preview_ready_ :
                MediaPreview().MediaPlayer() && MediaPreview().MediaPlayer().PlaybackSession().PlaybackState() !=
                    Windows::Media::Playback::MediaPlaybackState::Opening &&
                MediaPreview().MediaPlayer().PlaybackSession().PlaybackState() !=
                    Windows::Media::Playback::MediaPlaybackState::None;
            break;
        }
        return ready ? L"ready" : L"loading";
    }

    JsonObject MainWindow::CliSnapshot()
    {
        JsonObject result;
        result.SetNamedValue(L"id", JsonValue::CreateStringValue(cli_id_));
        result.SetNamedValue(L"generation", JsonValue::CreateStringValue(std::to_wstring(content_generation_)));
        result.SetNamedValue(L"visible", JsonValue::CreateBooleanValue(visible_));
        result.SetNamedValue(L"pinned", JsonValue::CreateBooleanValue(pinned_));
        result.SetNamedValue(L"topmost", JsonValue::CreateBooleanValue(topmost_));
        result.SetNamedValue(L"state", JsonValue::CreateStringValue(CliLoadState()));
        if (cli_control_generation_ == content_generation_)
        {
            if (cli_control_pending_) result.SetNamedValue(L"state", JsonValue::CreateStringValue(L"loading"));
            result.SetNamedValue(L"command_pending", JsonValue::CreateBooleanValue(cli_control_pending_));
            if (cli_control_error_)
            {
                result.SetNamedValue(L"command_error_code", JsonValue::CreateNumberValue(cli_control_error_));
                result.SetNamedValue(L"command_error_message", JsonValue::CreateStringValue(cli_control_error_message_));
            }
        }
        result.SetNamedValue(L"fallback", JsonValue::CreateBooleanValue(current_kind_ == glance::app::PreviewKind::generic));
        result.SetNamedValue(L"current_index", JsonValue::CreateNumberValue(current_index_));
        JsonArray paths;
        for (const auto& file : files_) paths.Append(JsonValue::CreateStringValue(file.path));
        result.SetNamedValue(L"paths", paths);
        RECT bounds{};
        GetWindowRect(window_, &bounds);
        JsonObject rectangle;
        rectangle.SetNamedValue(L"x", JsonValue::CreateNumberValue(bounds.left));
        rectangle.SetNamedValue(L"y", JsonValue::CreateNumberValue(bounds.top));
        rectangle.SetNamedValue(L"width", JsonValue::CreateNumberValue(bounds.right - bounds.left));
        rectangle.SetNamedValue(L"height", JsonValue::CreateNumberValue(bounds.bottom - bounds.top));
        result.SetNamedValue(L"bounds", rectangle);
        if (current_kind_ == glance::app::PreviewKind::text && !current_text_json_ && text_editor_)
        {
            result.SetNamedValue(L"line", JsonValue::CreateNumberValue(static_cast<double>(text_editor_->current_line())));
            result.SetNamedValue(L"line_count", JsonValue::CreateNumberValue(static_cast<double>(text_editor_->line_count())));
            result.SetNamedValue(L"has_more", JsonValue::CreateBooleanValue(current_text_has_more_));
        }
        if (current_kind_ == glance::app::PreviewKind::document)
        {
            result.SetNamedValue(L"page", JsonValue::CreateNumberValue(pdf_page_index_ + 1));
            result.SetNamedValue(L"page_count", JsonValue::CreateNumberValue(pdf_page_count_));
        }
        if (current_kind_ == glance::app::PreviewKind::media)
        {
            if (native_media_active_)
            {
                result.SetNamedValue(L"position", JsonValue::CreateNumberValue(native_media_state_.position_ticks / 10000000.0));
                result.SetNamedValue(L"duration", JsonValue::CreateNumberValue(native_media_state_.duration_ticks / 10000000.0));
                result.SetNamedValue(L"volume", JsonValue::CreateNumberValue(native_media_state_.volume_percent));
                result.SetNamedValue(L"muted", JsonValue::CreateBooleanValue((native_media_state_.flags & glance::contracts::native_preview::media_state_muted) != 0));
                result.SetNamedValue(L"playing", JsonValue::CreateBooleanValue((native_media_state_.flags & glance::contracts::native_preview::media_state_playing) != 0));
            }
            else if (const auto player = MediaPreview().MediaPlayer())
            {
                const auto session = player.PlaybackSession();
                result.SetNamedValue(L"position", JsonValue::CreateNumberValue(session.Position().count() / 10000000.0));
                result.SetNamedValue(L"duration", JsonValue::CreateNumberValue(session.NaturalDuration().count() / 10000000.0));
                result.SetNamedValue(L"volume", JsonValue::CreateNumberValue(player.Volume() * 100));
                result.SetNamedValue(L"muted", JsonValue::CreateBooleanValue(player.IsMuted()));
                result.SetNamedValue(L"playing", JsonValue::CreateBooleanValue(session.PlaybackState() == Windows::Media::Playback::MediaPlaybackState::Playing));
            }
        }
        return result;
    }

    bool MainWindow::CliControl(JsonObject const& request)
    {
        using glance::cli::Error;
        using Kind = glance::app::PreviewKind;
        if (!visible_) throw Error(8, "window_hidden", "Window has no active preview");
        const auto state = CliLoadState();
        if (state == L"failed") throw Error(9, "preview_failed", "Preview provider failed");
        if (state == L"loading") return false;
        const auto command = request.GetNamedString(L"command");
        const auto value = request.GetNamedNumber(L"value", 0);
        if (!std::isfinite(value)) throw Error(2, "invalid_number", "Expected a finite number");
        if (command == L"window.next" || command == L"window.previous")
        {
            const int step = command == L"window.next" ? 1 : -1;
            if (gallery_mode_ == GalleryMode::active)
            {
                if ((step < 0 && gallery_current_index_ == 0) ||
                    (step > 0 && gallery_total_known_ && gallery_current_index_ + 1 >= gallery_total_count_))
                    throw Error(3, "sequence_end", "No adjacent file");
                navigate_gallery(step);
            }
            else
            {
                const auto index = static_cast<std::int64_t>(current_index_) + step;
                if (index < 0 || index >= static_cast<std::int64_t>(files_.size())) throw Error(3, "sequence_end", "No adjacent file");
                FileList().SelectedIndex(static_cast<int>(index));
            }
            return true;
        }
        if (command == L"window.line")
        {
            if (current_kind_ != Kind::text || current_text_json_ || !text_editor_) throw Error(8, "content_type", "Expected a plain text preview");
            if (value < 1 || value > INT32_MAX || std::floor(value) != value) throw Error(2, "invalid_line", "Invalid line number");
            if (value > static_cast<double>(text_editor_->line_count()) ||
                (value == static_cast<double>(text_editor_->line_count()) && current_text_has_more_))
            {
                if (!current_text_has_more_) throw Error(3, "line_not_found", "Line is outside the document");
                load_next_text_chunk_async(content_generation_);
                return false;
            }
            text_editor_->go_to_line(static_cast<std::int64_t>(value));
            return true;
        }
        if (command == L"window.page")
        {
            if (current_kind_ != Kind::document) throw Error(8, "content_type", "Expected a PDF preview");
            if (value < 1 || value > pdf_page_count_ || std::floor(value) != value) throw Error(3, "page_not_found", "Page is outside the document");
            navigate_to_pdf_page(static_cast<std::uint32_t>(value - 1));
            return true;
        }
        if (current_kind_ != Kind::media) throw Error(8, "content_type", "Expected a media preview");
        const auto player = MediaPreview().MediaPlayer();
        if (command == L"window.seek")
        {
            const auto duration = native_media_active_ ? native_media_state_.duration_ticks / 10000000.0 : player.PlaybackSession().NaturalDuration().count() / 10000000.0;
            if (value < 0 || value > duration) throw Error(2, "invalid_position", "Position is outside the media duration");
            if (native_media_active_) send_native_media_control_async(native_preview_surface_, NativeMediaControl::seek, static_cast<std::int64_t>(value * 10000000));
            else player.PlaybackSession().Position(std::chrono::duration_cast<Windows::Foundation::TimeSpan>(std::chrono::duration<double>(value)));
        }
        else if (command == L"window.volume")
        {
            if (value < 0 || value > 100) throw Error(2, "invalid_volume", "Volume must be between 0 and 100");
            MediaVolumeSlider().Value(value);
        }
        else if (command == L"window.mute")
        {
            const bool enabled = request.GetNamedBoolean(L"enabled");
            const bool muted = native_media_active_ ? (native_media_state_.flags & glance::contracts::native_preview::media_state_muted) != 0 : player.IsMuted();
            if (enabled != muted) MediaMuteButton_Click(nullptr, nullptr);
        }
        else if (command == L"window.play" || command == L"window.pause")
        {
            const bool play = command == L"window.play";
            if (native_media_active_) send_native_media_control_async(native_preview_surface_, play ? NativeMediaControl::play : NativeMediaControl::pause, 0);
            else if (play) player.Play(); else player.Pause();
        }
        else throw Error(2, "unknown_command", "Unknown content control");
        return true;
    }

    void MainWindow::CliExecuteControl(JsonObject const& request)
    {
        if (cli_control_pending_ && cli_control_generation_ == content_generation_)
            throw glance::cli::Error(8, "control_pending", "A content control is still pending");
        cli_control_generation_ = content_generation_;
        cli_control_error_ = 0;
        cli_control_pending_ = !CliControl(request);
        if (cli_control_pending_) cli_control_async(request, content_generation_);
    }

    fire_and_forget MainWindow::cli_control_async(JsonObject request, std::uint64_t generation)
    {
        const auto weak = get_weak();
        const apartment_context ui;
        try
        {
            for (;;)
            {
                co_await resume_after(std::chrono::milliseconds(50));
                co_await ui;
                const auto self = weak.get();
                if (!self || !self->visible_ || self->content_generation_ != generation) co_return;
                if (self->CliControl(request))
                {
                    self->cli_control_pending_ = false;
                    co_return;
                }
            }
        }
        catch (const glance::cli::Error& error)
        {
            if (const auto self = weak.get(); self && self->content_generation_ == generation)
            {
                self->cli_control_pending_ = false;
                self->cli_control_error_ = error.code;
                self->cli_control_error_message_ = to_hstring(error.what());
            }
        }
        catch (...)
        {
            if (const auto self = weak.get(); self && self->content_generation_ == generation)
            {
                self->cli_control_pending_ = false;
                self->cli_control_error_ = 1;
                self->cli_control_error_message_ = L"Content control failed";
            }
        }
    }

    void MainWindow::CliTopmost(bool enabled)
    {
        if (pinned_ && !enabled) throw glance::cli::Error(8, "pinned_topmost", "Pinned windows must be topmost");
        if (!visible_) throw glance::cli::Error(8, "window_hidden", "Target window has no active preview; use 'windows' to find the visible window, then pass --id ID");
        topmost_ = enabled;
        TopmostButton().IsChecked(enabled);
        set_topmost(enabled);
        update_state();
    }

    void MainWindow::CliPin(bool enabled)
    {
        if (!visible_) throw glance::cli::Error(8, "window_hidden", "Target window has no active preview; use 'windows' to find the visible window, then pass --id ID");
        if (pinned_ == enabled) return;
        PinButton().IsChecked(enabled);
        PinButton_Click(nullptr, nullptr);
    }

    void MainWindow::CliConfigure(JsonObject const& options, bool validate_only)
    {
        using glance::cli::Error;
        if (options.HasKey(L"topmost") && options.GetNamedBoolean(L"pin", false) && !options.GetNamedBoolean(L"topmost"))
            throw Error(8, "pinned_topmost", "Pinned windows must be topmost");
        if (options.HasKey(L"close_after"))
        {
            const auto seconds = options.GetNamedNumber(L"close_after");
            if (!std::isfinite(seconds) || seconds <= 0 || seconds > 86400) throw Error(2, "invalid_delay", "Invalid close delay");
        }
        const bool geometry = options.HasKey(L"size") || options.HasKey(L"position") || options.HasKey(L"center_offset");
        if (geometry && !validate_only && !visible_)
            throw Error(8, "window_hidden", "Target window has no active preview; use 'windows' to find the visible window, then pass --id ID");
        RECT bounds{};
        GetWindowRect(window_, &bounds);
        int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
        int x = bounds.left, y = bounds.top;
        const auto pair = [&](const wchar_t* key) {
            auto values = options.GetNamedArray(key);
            if (values.Size() != 2) throw Error(2, "invalid_geometry", "Expected two coordinates");
            const double a = values.GetNumberAt(0), b = values.GetNumberAt(1);
            if (!std::isfinite(a) || !std::isfinite(b) || std::floor(a) != a || std::floor(b) != b ||
                std::abs(a) > 1000000 || std::abs(b) > 1000000) throw Error(2, "invalid_geometry", "Invalid coordinates");
            return POINT{ static_cast<LONG>(a), static_cast<LONG>(b) };
        };
        if (options.HasKey(L"size"))
        {
            auto size = pair(L"size");
            const int dpi = static_cast<int>(GetDpiForWindow(window_));
            if (size.x < MulDiv(480, dpi, 96) || size.y < MulDiv(320, dpi, 96) || size.x > 32767 || size.y > 32767)
                throw Error(2, "invalid_size", "Size is outside the window limits");
            width = size.x; height = size.y;
        }
        if (options.HasKey(L"position")) { auto position = pair(L"position"); x = position.x; y = position.y; }
        if (options.HasKey(L"center_offset"))
        {
            std::vector<HMONITOR> monitors;
            EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM context) -> BOOL {
                reinterpret_cast<std::vector<HMONITOR>*>(context)->push_back(monitor); return TRUE;
            }, reinterpret_cast<LPARAM>(&monitors));
            HMONITOR monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
            if (options.GetNamedString(L"command", L"") == L"preview")
            {
                POINT cursor{}; GetCursorPos(&cursor);
                monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
            }
            if (options.HasKey(L"monitor"))
            {
                const auto index = options.GetNamedNumber(L"monitor");
                if (index < 0 || index >= static_cast<double>(monitors.size()) || std::floor(index) != index) throw Error(3, "monitor_not_found", "Monitor not found");
                monitor = monitors[static_cast<std::size_t>(index)];
            }
            MONITORINFO info{ sizeof(info) };
            if (!GetMonitorInfoW(monitor, &info)) throw Error(3, "monitor_not_found", "Monitor unavailable");
            const auto offset = pair(L"center_offset");
            x = info.rcWork.left + (info.rcWork.right - info.rcWork.left - width) / 2 + offset.x;
            y = info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - height) / 2 + offset.y;
        }
        if (validate_only) return;
        if (geometry)
        {
            if (!SetWindowPos(window_, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE))
                throw Error(1, "placement_failed", "Cannot set window placement");
            cli_explicit_geometry_ = true;
            component_placement_generation_ = 0;
            reveal_deferred_preview();
        }
        if (options.HasKey(L"topmost")) CliTopmost(options.GetNamedBoolean(L"topmost"));
        if (options.GetNamedBoolean(L"pin", false)) CliPin(true);
        if (options.HasKey(L"close_after"))
        {
            const auto seconds = options.GetNamedNumber(L"close_after");
            if (!std::isfinite(seconds) || seconds <= 0 || seconds > 86400) throw Error(2, "invalid_delay", "Invalid close delay");
            if (cli_close_timer_) cli_close_timer_.Stop();
            cli_close_timer_ = Microsoft::UI::Xaml::DispatcherTimer();
            cli_close_timer_.Interval(std::chrono::milliseconds(50));
            cli_close_timer_.Tick([weak = get_weak(), generation = content_generation_,
                delay = static_cast<ULONGLONG>(seconds * 1000), deadline = ULONGLONG{}](auto const& sender, auto const&) mutable {
                auto timer = sender.template as<Microsoft::UI::Xaml::DispatcherTimer>();
                const auto self = weak.get();
                if (!self || self->content_generation_ != generation || !self->visible_ || self->CliLoadState() == L"failed")
                { timer.Stop(); return; }
                if (!deadline && self->CliLoadState() == L"ready") deadline = GetTickCount64() + delay;
                if (deadline && GetTickCount64() >= deadline)
                { timer.Stop(); if (self->detached_) self->CloseForReplacement(); else self->HidePreview(); }
            });
            cli_close_timer_.Start();
        }
    }
}

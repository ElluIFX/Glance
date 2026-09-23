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
        if (ErrorText().Visibility() == Visibility::Visible && !ErrorText().Text().empty()) return L"failed";
        using Kind = glance::app::PreviewKind;
        bool ready = false;
        switch (current_kind_)
        {
        case Kind::generic: ready = true; break;
        case Kind::component: break;
        case Kind::image: ready = image_pixel_width_ > 0 && ImagePreview().Source() != nullptr; break;
        case Kind::document: ready = PdfPageImage().Source() != nullptr; break;
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
        return result;
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

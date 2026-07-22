#include "pch.h"
#include "MainWindow.xaml.h"
#include "appearance_preferences.h"
#include "generic_file_info.h"
#include "image_metadata_provider.h"
#include "localization.h"
#include "markdown_renderer.h"
#include "media_metadata_provider.h"
#include "path_copy_preferences.h"
#include "resource.h"
#include "syntax_highlighter.h"
#include "window_size_store.h"
#include "glance/contracts/diagnostics.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <microsoft.ui.xaml.window.h>
#include <dwrite.h>
#include <shellapi.h>
#include <shlobj_core.h>
#include <shlwapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Documents;
using namespace Microsoft::UI::Xaml::Input;

namespace
{
    std::filesystem::path executable_directory()
    {
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

    std::wstring quote_command_line_argument(std::wstring_view value)
    {
        std::wstring quoted{ L'"' };
        std::size_t backslashes{};
        for (const wchar_t character : value)
        {
            if (character == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (character == L'"')
            {
                quoted.append(backslashes * 2 + 1, L'\\');
                quoted.push_back(character);
                backslashes = 0;
                continue;
            }
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(character);
        }
        quoted.append(backslashes * 2, L'\\');
        quoted.push_back(L'"');
        return quoted;
    }

    bool is_audio_path(std::wstring_view path)
    {
        auto extension = std::filesystem::path(path).extension().wstring();
        std::ranges::transform(extension, extension.begin(), [](wchar_t value) {
            return static_cast<wchar_t>(std::towlower(value));
        });
        static constexpr std::array audio_extensions{
            std::wstring_view(L".mp3"), std::wstring_view(L".flac"), std::wstring_view(L".wav"),
            std::wstring_view(L".m4a"), std::wstring_view(L".aac"), std::wstring_view(L".ogg") };
        return std::ranges::find(audio_extensions, extension) != audio_extensions.end();
    }

    struct SyntaxPalette
    {
        std::array<std::uint32_t, 5> light;
        std::array<std::uint32_t, 5> dark;
    };

    constexpr std::array syntax_palettes{
        SyntaxPalette{ { 0x005FB8, 0x107C10, 0x6B6B6B, 0x9C6500, 0xA4262C }, { 0x569CD6, 0xCE9178, 0x6A9955, 0xB5CEA8, 0xC586C0 } },
        SyntaxPalette{ { 0x0000FF, 0xA31515, 0x008000, 0x098658, 0x800080 }, { 0x569CD6, 0xCE9178, 0x6A9955, 0xB5CEA8, 0xC586C0 } },
        SyntaxPalette{ { 0xC2185B, 0x7A5D00, 0x6B6B63, 0x6F42C1, 0x007C91 }, { 0xF92672, 0xE6DB74, 0x75715E, 0xAE81FF, 0x66D9EF } },
        SyntaxPalette{ { 0xCF222E, 0x0A3069, 0x6E7781, 0x0550AE, 0x8250DF }, { 0xFF7B72, 0xA5D6FF, 0x8B949E, 0x79C0FF, 0xD2A8FF } },
        SyntaxPalette{ { 0xC2185B, 0x6F5B00, 0x607090, 0x6A4C93, 0x007C91 }, { 0xFF79C6, 0xF1FA8C, 0x6272A4, 0xBD93F9, 0x8BE9FD } },
        SyntaxPalette{ { 0x859900, 0x2AA198, 0x93A1A1, 0xD33682, 0x268BD2 }, { 0xB5BD00, 0x2AA198, 0x839496, 0xD33682, 0x268BD2 } },
        SyntaxPalette{ { 0x8F5F86, 0x4F7D4A, 0x6A7280, 0xA44A3F, 0x3B6EA5 }, { 0xB48EAD, 0xA3BE8C, 0x616E88, 0xD08770, 0x81A1C1 } },
        SyntaxPalette{ { 0xA626A4, 0x50A14F, 0xA0A1A7, 0x986801, 0x4078F2 }, { 0xC678DD, 0x98C379, 0x5C6370, 0xD19A66, 0x61AFEF } },
        SyntaxPalette{ { 0xCC241D, 0x79740E, 0x928374, 0x8F3F71, 0x076678 }, { 0xFB4934, 0xB8BB26, 0x928374, 0xD3869B, 0x83A598 } },
        SyntaxPalette{ { 0x8959A8, 0x718C00, 0x8E908C, 0xF5871F, 0x4271AE }, { 0xB294BB, 0xB5BD68, 0x969896, 0xDE935F, 0x81A2BE } },
    };
    static_assert(
        syntax_palettes.size() ==
        static_cast<std::size_t>(glance::app::SyntaxThemePreference::tomorrow_night) + 1);

    Media::Brush syntax_brush(
        glance::app::SyntaxStyle style,
        glance::app::SyntaxThemePreference theme,
        bool dark)
    {
        std::size_t style_index{};
        switch (style)
        {
        case glance::app::SyntaxStyle::keyword: style_index = 0; break;
        case glance::app::SyntaxStyle::string: style_index = 1; break;
        case glance::app::SyntaxStyle::comment: style_index = 2; break;
        case glance::app::SyntaxStyle::number: style_index = 3; break;
        case glance::app::SyntaxStyle::directive: style_index = 4; break;
        default: return nullptr;
        }

        const auto theme_index = std::min<std::size_t>(
            static_cast<std::uint32_t>(theme),
            syntax_palettes.size() - 1);
        const auto& palette = syntax_palettes[theme_index];
        const std::uint32_t rgb = (dark ? palette.dark : palette.light)[style_index];
        const Windows::UI::Color color{
            255,
            static_cast<std::uint8_t>((rgb >> 16U) & 0xFFU),
            static_cast<std::uint8_t>((rgb >> 8U) & 0xFFU),
            static_cast<std::uint8_t>(rgb & 0xFFU) };
        return Media::SolidColorBrush(color);
    }
}

namespace winrt::Glance::App::implementation
{
    MainWindow::MainWindow()
    {
        glance::contracts::log_event(L"MainWindow InitializeComponent begin.");
        InitializeComponent();
        glance::contracts::log_event(L"MainWindow InitializeComponent complete.");
        ApplyLocalizedResources();
        ApplyAppearancePreferences();
        configure_window();
        glance::contracts::log_event(L"MainWindow native configuration complete.");
        media_timer_ = DispatcherTimer();
        media_timer_.Interval(std::chrono::milliseconds(250));
        const auto weak = get_weak();
        media_timer_.Tick([weak](IInspectable const&, IInspectable const&) {
            if (const auto self = weak.get())
            {
                self->update_media_controls();
            }
        });
    }

    void MainWindow::InitializeSession(std::uint64_t instance_id, StateCallback callback)
    {
        instance_id_ = instance_id;
        state_callback_ = std::move(callback);
    }

    void MainWindow::ApplyAppearancePreferences()
    {
        RootGrid().RequestedTheme(glance::app::element_theme(
            glance::app::load_appearance_preferences().theme));
        if (!current_text_.empty())
        {
            render_text_content();
            if (current_text_markdown_)
            {
                render_markdown();
            }
        }
    }

    void MainWindow::ApplyLocalizedResources()
    {
        const auto set_tooltip = [](const auto& control, wchar_t const* key) {
            ToolTipService::SetToolTip(control, box_value(glance::app::localize(key)));
        };

        set_tooltip(TopmostButton(), L"TopmostButton.ToolTipService.ToolTip");
        set_tooltip(PinButton(), L"PinButton.ToolTipService.ToolTip");
        set_tooltip(ClosePreviewButton(), L"ClosePreviewButton.ToolTipService.ToolTip");
        LoadCloudFileText().Text(glance::app::localize(L"LoadCloudFileText.Text"));
        set_tooltip(MediaPlayPauseButton(), L"MediaPlayPauseButton.ToolTipService.ToolTip");
        set_tooltip(MediaMuteButton(), L"MediaMuteButton.ToolTipService.ToolTip");
        set_tooltip(PreviousPdfButton(), L"PreviousPdfButton.ToolTipService.ToolTip");
        set_tooltip(NextPdfButton(), L"NextPdfButton.ToolTipService.ToolTip");
        ArchiveNameHeader().Text(glance::app::localize(L"ArchiveNameHeader.Text"));
        ArchiveTypeHeader().Text(glance::app::localize(L"ArchiveTypeHeader.Text"));
        ArchiveModifiedHeader().Text(glance::app::localize(L"ArchiveModifiedHeader.Text"));
        ArchiveSizeHeader().Text(glance::app::localize(L"ArchiveSizeHeader.Text"));
        MarkdownPreviewButton().Content(box_value(glance::app::localize(L"MarkdownPreviewButton.Content")));
        MarkdownCodeButton().Content(box_value(glance::app::localize(L"MarkdownCodeButton.Content")));
        SystemAnsiItem().Text(glance::app::localize(L"SystemAnsiItem.Text"));
        set_tooltip(SyntaxHighlightButton(), L"SyntaxHighlightButton.ToolTipService.ToolTip");
        set_tooltip(ZoomOutButton(), L"ZoomOutButton.ToolTipService.ToolTip");
        set_tooltip(ZoomInButton(), L"ZoomInButton.ToolTipService.ToolTip");
        set_tooltip(RotateButton(), L"RotateButton.ToolTipService.ToolTip");
        set_tooltip(ImageExifButton(), L"ImageExifButton.ToolTipService.ToolTip");
        set_tooltip(WordWrapButton(), L"WordWrapButton.ToolTipService.ToolTip");
        set_tooltip(CopyPathButton(), L"CopyPathButton.ToolTipService.ToolTip");
        set_tooltip(OpenDefaultButton(), L"OpenDefaultButton.ToolTipService.ToolTip");
        set_tooltip(OpenFolderButton(), L"OpenFolderButton.ToolTipService.ToolTip");
        if (files_.empty())
        {
            EncodingSelector().Content(box_value(glance::app::localize(L"EncodingSelector.Content")));
        }
        if (!TextEncodingText().Text().empty())
        {
            TextEncodingText().Text(glance::app::localize(L"PreviewTruncated"));
        }
        update_line_number_visibility();
    }

    void MainWindow::ApplyTextPreferences()
    {
        apply_text_preferences();
        if (current_kind_ == glance::app::PreviewKind::text ||
            current_kind_ == glance::app::PreviewKind::markdown)
        {
            render_text_content();
            update_text_layout();
        }
    }

    void MainWindow::configure_window()
    {
        glance::contracts::log_event(L"Resolving the native window handle.");
        const auto window_native = this->try_as<::IWindowNative>();
        check_hresult(window_native->get_WindowHandle(&window_));

        if (const HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_GLANCE_APP)))
        {
            SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
            SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        }

        glance::contracts::log_event(L"Applying native window styles.");
        LONG_PTR extended_style = GetWindowLongPtrW(window_, GWL_EXSTYLE);
        extended_style |= WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
        SetWindowLongPtrW(window_, GWL_EXSTYLE, extended_style);

        LONG_PTR window_style = GetWindowLongPtrW(window_, GWL_STYLE);
        window_style &= ~(WS_SYSMENU | WS_MINIMIZEBOX);
        SetWindowLongPtrW(window_, GWL_STYLE, window_style);
        SetWindowSubclass(window_, window_subclass, 1, reinterpret_cast<DWORD_PTR>(this));

        if (const auto presenter = AppWindow().Presenter().try_as<Microsoft::UI::Windowing::OverlappedPresenter>())
        {
            presenter.IsMinimizable(false);
            presenter.IsMaximizable(true);
            presenter.SetBorderAndTitleBar(true, false);
        }

        glance::contracts::log_event(L"Enabling the custom title bar.");
        ExtendsContentIntoTitleBar(true);
        glance::contracts::log_event(L"Assigning the title bar drag region.");
        SetTitleBar(TitleBarDragRegion());
        glance::contracts::log_event(L"Refreshing the native window frame.");
        SetWindowPos(
            window_,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        glance::contracts::log_event(L"Assigning the window title.");
        const std::wstring window_title = glance::app::localize(L"AppName");
        SetWindowTextW(window_, window_title.c_str());
        glance::contracts::log_event(L"Priming the hidden preview window.");
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        ShowWindow(window_, SW_HIDE);
    }

    LRESULT CALLBACK MainWindow::window_subclass(
        HWND window,
        UINT message,
        WPARAM wparam,
        LPARAM lparam,
        UINT_PTR,
        DWORD_PTR reference_data) noexcept
    {
        auto* self = reinterpret_cast<MainWindow*>(reference_data);
        if (message == WM_MOUSEACTIVATE)
        {
            return MA_NOACTIVATE;
        }
        if (message == WM_GETMINMAXINFO)
        {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            const UINT dpi = GetDpiForWindow(window);
            limits->ptMinTrackSize.x = MulDiv(480, static_cast<int>(dpi), 96);
            limits->ptMinTrackSize.y = MulDiv(320, static_cast<int>(dpi), 96);
            return 0;
        }
        if (message == WM_EXITSIZEMOVE && self != nullptr)
        {
            self->user_sized_ = true;
            self->save_current_window_size();
        }
        if (message == WM_SYSCOMMAND && self != nullptr &&
            (wparam & 0xFFF0U) == SC_MAXIMIZE)
        {
            self->user_sized_ = true;
        }
        if (message == WM_NCDESTROY && self != nullptr)
        {
            glance::contracts::log_event(L"MainWindow received WM_NCDESTROY.");
            RemoveWindowSubclass(window, window_subclass, 1);
            self->stop_detached_focus_monitor();
            if (self->media_timer_ != nullptr)
            {
                self->media_timer_.Stop();
            }
            if (!self->office_temp_pdf_.empty())
            {
                DeleteFileW(self->office_temp_pdf_.c_str());
                self->office_temp_pdf_.clear();
            }
            self->state_ = glance::contracts::PreviewWindowState::closed;
            if (self->state_callback_)
            {
                self->state_callback_(self->instance_id_, self->state_);
            }
        }
        return DefSubclassProc(window, message, wparam, lparam);
    }

    void MainWindow::ShowPreview(
        std::vector<glance::app::PreviewFile> files,
        std::uint32_t focused_index,
        std::uint32_t source_kind,
        HWND source_window)
    {
        const bool new_session = !visible_;
        stop_detached_focus_monitor();
        files_ = std::move(files);
        source_kind_ = source_kind;
        source_window_ = source_window;
        current_index_ = files_.empty() ? 0 : std::min<std::uint32_t>(focused_index, static_cast<std::uint32_t>(files_.size() - 1));
        if (new_session)
        {
            topmost_ = false;
            pinned_ = false;
            detached_ = false;
            user_sized_ = false;
            TopmostButton().IsChecked(false);
            PinButton().IsChecked(false);
        }
        visible_ = true;

        FileList().Items().Clear();
        for (const auto& file : files_)
        {
            FileList().Items().Append(box_value(file.display_name));
        }
        if (files_.size() > 1)
        {
            FileListColumn().Width(GridLength{ 220, GridUnitType::Pixel });
            FileList().Visibility(Visibility::Visible);
            FileList().SelectedIndex(static_cast<int>(current_index_));
        }
        else
        {
            FileListColumn().Width(GridLength{ 0, GridUnitType::Pixel });
            FileList().Visibility(Visibility::Collapsed);
        }

        present_file(current_index_);
        if (new_session || !user_sized_)
        {
            position_initial_window();
        }
        update_state();
    }

    void MainWindow::HidePreview()
    {
        if (!visible_ || detached_)
        {
            return;
        }
        visible_ = false;
        ShowWindow(window_, SW_HIDE);
        clear_preview_content();
        reset_hidden_window_size();
        state_ = glance::contracts::PreviewWindowState::hidden;
        if (state_callback_)
        {
            state_callback_(instance_id_, state_);
        }
    }

    void MainWindow::clear_preview_content()
    {
        ++content_generation_;
        stop_media_playback();

        ImagePreview().Source(nullptr);
        ImageMetadataText().Text(L"");
        ImageMetadataOverlay().Visibility(Visibility::Collapsed);
        MediaCoverImage().Source(nullptr);
        MediaTitleText().Text(L"");
        MediaAlbumText().Text(L"");
        MediaArtistText().Text(L"");
        MediaTimeText().Text(L"0:00 / 0:00");
        PdfPageImage().Source(nullptr);
        PdfLoadingOverlay().Visibility(Visibility::Collapsed);
        PdfPageText().Text(L"");
        pdf_document_ = nullptr;
        MarkdownPreviewWebView().Opacity(0.0);
        MarkdownPreviewWebView().Visibility(Visibility::Collapsed);
        TextContentRichText().Blocks().Clear();
        LineNumberText().Text(L"");
        TextEncodingText().Text(L"");
        if (font_size_overlay_timer_ != nullptr)
        {
            font_size_overlay_timer_.Stop();
        }
        TextFontSizeOverlay().Visibility(Visibility::Collapsed);
        ArchiveEntryList().Items().Clear();
        ArchiveStatusText().Text(L"");
        FileList().Items().Clear();
        FileList().Visibility(Visibility::Collapsed);
        FileListColumn().Width(GridLength{ 0, GridUnitType::Pixel });

        TitleText().Text(L"");
        FooterMetadataText().Text(L"");
        FileNameText().Text(L"");
        FilePathText().Text(L"");
        FileMetadataText().Text(L"");
        GenericAdvancedInfoText().Text(L"");
        GenericAdvancedInfoScroller().Visibility(Visibility::Collapsed);
        LoadCloudFileButton().Visibility(Visibility::Collapsed);
        ErrorText().Text(L"");
        ErrorText().Visibility(Visibility::Collapsed);
        GenericPanel().Visibility(Visibility::Visible);
        TextPanel().Visibility(Visibility::Collapsed);
        ImagePanel().Visibility(Visibility::Collapsed);
        MediaPanel().Visibility(Visibility::Collapsed);
        PdfPanel().Visibility(Visibility::Collapsed);
        ArchivePanel().Visibility(Visibility::Collapsed);
        TextStatusControls().Visibility(Visibility::Collapsed);
        ImageStatusControls().Visibility(Visibility::Collapsed);
        SyntaxHighlightButton().Visibility(Visibility::Collapsed);
        WordWrapButton().Visibility(Visibility::Collapsed);
        LineNumbersButton().Visibility(Visibility::Collapsed);

        current_text_.clear();
        current_text_path_.clear();
        current_text_markdown_ = false;
        image_metadata_.clear();
        media_dimensions_.clear();
        media_technical_info_.clear();
        files_.clear();
        current_index_ = 0;
        source_kind_ = 0;
        source_window_ = nullptr;
        foreground_when_unpinned_ = nullptr;
        current_kind_ = glance::app::PreviewKind::generic;
        media_is_audio_ = false;
        image_metadata_visible_ = false;
        image_panning_ = false;
        pdf_page_index_ = 0;
        pdf_wheel_delta_ = 0;

    }

    void MainWindow::reset_hidden_window_size() noexcept
    {
        if (window_ == nullptr)
        {
            return;
        }
        const UINT dpi = GetDpiForWindow(window_);
        SetWindowPos(
            window_,
            nullptr,
            0,
            0,
            MulDiv(720, static_cast<int>(dpi), 96),
            MulDiv(520, static_cast<int>(dpi), 96),
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void MainWindow::position_initial_window(bool ignore_saved_size)
    {
        HMONITOR monitor = MonitorFromWindow(source_window_ != nullptr ? source_window_ : GetForegroundWindow(), MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{ sizeof(MONITORINFO) };
        GetMonitorInfoW(monitor, &info);

        const UINT dpi = source_window_ != nullptr ? GetDpiForWindow(source_window_) : 96;
        int desired_width = MulDiv(files_.size() > 1 ? 920 : 720, static_cast<int>(dpi), 96);
        int desired_height = MulDiv(520, static_cast<int>(dpi), 96);
        if (!ignore_saved_size && !auto_fit_applies())
        {
            if (const auto saved_size = glance::app::load_window_size(current_kind_, media_is_audio_))
            {
                desired_width = MulDiv(saved_size->cx, static_cast<int>(dpi), 96);
                desired_height = MulDiv(saved_size->cy, static_cast<int>(dpi), 96);
            }
        }
        const int work_width = info.rcWork.right - info.rcWork.left;
        const int work_height = info.rcWork.bottom - info.rcWork.top;
        const int minimum_width = MulDiv(480, static_cast<int>(dpi), 96);
        const int minimum_height = MulDiv(320, static_cast<int>(dpi), 96);
        const int width = std::clamp(desired_width, std::min(minimum_width, work_width), work_width);
        const int height = std::clamp(desired_height, std::min(minimum_height, work_height), work_height);
        const int x = info.rcWork.left + (work_width - width) / 2;
        const int y = info.rcWork.top + (work_height - height) / 2;

        SetWindowPos(
            window_,
            HWND_TOPMOST,
            x,
            y,
            width,
            height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (!topmost_)
        {
            SetWindowPos(
                window_,
                HWND_NOTOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }

    bool MainWindow::auto_fit_applies() const noexcept
    {
        if (!glance::app::auto_fit_window_size_enabled())
        {
            return false;
        }
        return current_kind_ == glance::app::PreviewKind::image ||
            current_kind_ == glance::app::PreviewKind::pdf ||
            current_kind_ == glance::app::PreviewKind::office ||
            (current_kind_ == glance::app::PreviewKind::media && !media_is_audio_);
    }

    void MainWindow::auto_fit_window_to_content(double content_width, double content_height) noexcept
    {
        if (!auto_fit_applies() || window_ == nullptr ||
            content_width <= 0.0 || content_height <= 0.0)
        {
            return;
        }

        RECT bounds{};
        if (!GetWindowRect(window_, &bounds))
        {
            return;
        }
        HMONITOR monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{ sizeof(MONITORINFO) };
        if (!GetMonitorInfoW(monitor, &info))
        {
            return;
        }

        FrameworkElement panel = current_kind_ == glance::app::PreviewKind::image
            ? ImagePanel().as<FrameworkElement>()
            : current_kind_ == glance::app::PreviewKind::media
                ? MediaPanel().as<FrameworkElement>()
                : PdfPanel().as<FrameworkElement>();
        const int current_width = bounds.right - bounds.left;
        const int current_height = bounds.bottom - bounds.top;
        const int horizontal_chrome = std::max(0, current_width - static_cast<int>(std::lround(panel.ActualWidth())));
        const int vertical_chrome = std::max(0, current_height - static_cast<int>(std::lround(panel.ActualHeight())));
        const int work_width = info.rcWork.right - info.rcWork.left;
        const int work_height = info.rcWork.bottom - info.rcWork.top;
        const int maximum_width = std::max(1, static_cast<int>(std::floor(work_width * 0.75)));
        const int maximum_height = std::max(1, static_cast<int>(std::floor(work_height * 0.75)));
        const UINT dpi = GetDpiForWindow(window_);
        const int minimum_width = std::min(maximum_width, MulDiv(480, static_cast<int>(dpi), 96));
        const int minimum_height = std::min(maximum_height, MulDiv(320, static_cast<int>(dpi), 96));

        const double maximum_content_width = std::max(1, maximum_width - horizontal_chrome);
        const double maximum_content_height = std::max(1, maximum_height - vertical_chrome);
        const double minimum_content_width = std::max(1, minimum_width - horizontal_chrome);
        const double minimum_content_height = std::max(1, minimum_height - vertical_chrome);
        const double lower_scale = std::max(
            minimum_content_width / content_width,
            minimum_content_height / content_height);
        const double upper_scale = std::min(
            maximum_content_width / content_width,
            maximum_content_height / content_height);
        const double scale = lower_scale > upper_scale
            ? upper_scale
            : std::clamp(1.0, lower_scale, upper_scale);
        const int width = std::clamp(
            static_cast<int>(std::lround(content_width * scale)) + horizontal_chrome,
            minimum_width,
            maximum_width);
        const int height = std::clamp(
            static_cast<int>(std::lround(content_height * scale)) + vertical_chrome,
            minimum_height,
            maximum_height);
        const int x = info.rcWork.left + (work_width - width) / 2;
        const int y = info.rcWork.top + (work_height - height) / 2;
        SetWindowPos(window_, nullptr, x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
    }

    void MainWindow::save_current_window_size() const noexcept
    {
        if (!visible_ || window_ == nullptr || IsZoomed(window_) || auto_fit_applies())
        {
            return;
        }
        RECT bounds{};
        if (!GetWindowRect(window_, &bounds))
        {
            return;
        }
        const UINT dpi = GetDpiForWindow(window_);
        glance::app::save_window_size(
            current_kind_,
            SIZE{
                MulDiv(bounds.right - bounds.left, 96, static_cast<int>(dpi)),
                MulDiv(bounds.bottom - bounds.top, 96, static_cast<int>(dpi)) },
            media_is_audio_);
    }

    void MainWindow::present_file(std::uint32_t index)
    {
        if (index >= files_.size())
        {
            return;
        }
        current_index_ = index;
        const auto& file = files_[index];
        const auto generation = ++content_generation_;
        TitleText().Text(file.display_name);

        const auto size = formatted_size(file.size);
        const auto time = formatted_time(file.last_write_time);
        FooterMetadataText().Text(size + L"  |  " + time);

        const bool from_explorer = source_kind_ == 1;
        OpenFolderButton().Visibility(from_explorer ? Visibility::Collapsed : Visibility::Visible);
        OpenDefaultButton().Visibility(file.path.empty() ? Visibility::Collapsed : Visibility::Visible);

        if (file.is_cloud_placeholder || file.path.empty())
        {
            current_kind_ = glance::app::PreviewKind::generic;
            present_generic(file);
            return;
        }

        if ((file.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            current_kind_ = glance::app::PreviewKind::archive;
            show_content_panel(current_kind_);
            ArchiveStatusText().Text(glance::app::localize(L"LoadingFolder"));
            ArchiveEntryList().Items().Clear();
            load_directory_async(file.path, generation);
            return;
        }

        const auto kind = glance::app::resolve_preview_kind(file.path);
        current_kind_ = kind;
        switch (kind)
        {
        case glance::app::PreviewKind::text:
            present_text(file, false);
            break;
        case glance::app::PreviewKind::markdown:
            present_text(file, true);
            break;
        case glance::app::PreviewKind::image:
            show_content_panel(kind);
            image_rotation_ = 0;
            image_panning_ = false;
            image_metadata_.clear();
            image_metadata_visible_ = false;
            ImageTransform().Rotation(image_rotation_);
            ImagePreview().Source(nullptr);
            ImageExifButton().IsChecked(false);
            ImageMetadataText().Text(L"");
            ImageMetadataOverlay().Visibility(Visibility::Collapsed);
            static_cast<void>(ImageScroller().ChangeView(nullptr, nullptr, 1.0F, true));
            load_image_async(file.path, generation);
            load_image_metadata_async(file.path, generation);
            break;
        case glance::app::PreviewKind::media:
            show_content_panel(kind);
            media_is_audio_ = is_audio_path(file.path);
            MediaCoverImage().Source(nullptr);
            MediaCoverImage().Visibility(Visibility::Collapsed);
            MediaCoverPlaceholder().Visibility(Visibility::Visible);
            MediaTitleText().Text(file.display_name);
            MediaAlbumText().Text(L"");
            MediaArtistText().Text(L"");
            media_dimensions_.clear();
            media_technical_info_.clear();
            media_controls_idle_ticks_ = 0;
            show_media_controls();
            load_media_async(file.path, generation);
            break;
        case glance::app::PreviewKind::pdf:
            show_content_panel(kind);
            pdf_wheel_delta_ = 0;
            PdfPageText().Text(glance::app::localize(L"Loading"));
            PdfLoadingText().Text(glance::app::localize(L"LoadingPdf"));
            PdfLoadingOverlay().Visibility(Visibility::Visible);
            load_pdf_async(file.path, generation);
            break;
        case glance::app::PreviewKind::archive:
            show_content_panel(kind);
            ArchiveStatusText().Text(glance::app::localize(L"LoadingArchive"));
            ArchiveEntryList().Items().Clear();
            load_archive_async(file.path, generation);
            break;
        case glance::app::PreviewKind::office:
            show_content_panel(glance::app::PreviewKind::pdf);
            pdf_document_ = nullptr;
            PdfPageImage().Source(nullptr);
            pdf_wheel_delta_ = 0;
            PdfPageText().Text(L"1 / 1");
            PdfLoadingText().Text(glance::app::localize(L"ConvertingOffice"));
            PdfLoadingOverlay().Visibility(Visibility::Visible);
            load_office_async(file.path, generation, file.size, file.last_write_time);
            break;
        default:
            present_generic(file);
            break;
        }
    }

    void MainWindow::present_generic(const glance::app::PreviewFile& file)
    {
        current_kind_ = glance::app::PreviewKind::generic;
        show_content_panel(glance::app::PreviewKind::generic);
        FileNameText().Text(file.display_name);
        FilePathText().Text(!file.path.empty() ? file.path : file.parsing_name);
        FileMetadataText().Text(formatted_size(file.size) + L"  |  " + formatted_time(file.last_write_time));
        GenericAdvancedInfoText().Text(L"");
        GenericAdvancedInfoScroller().Visibility(Visibility::Collapsed);
        LoadCloudFileButton().Visibility(file.is_cloud_placeholder ? Visibility::Visible : Visibility::Collapsed);
        ErrorText().Visibility(Visibility::Collapsed);
        if (!file.path.empty() && !file.is_cloud_placeholder &&
            (file.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            load_generic_file_info_async(file.path, content_generation_);
        }
    }

    fire_and_forget MainWindow::load_generic_file_info_async(std::wstring path, std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        co_await resume_background();
        auto info = glance::app::load_generic_file_info(path);
        static_cast<void>(dispatcher.TryEnqueue([lifetime, generation, info = std::move(info)]() mutable {
            if (generation != lifetime->content_generation_ || info.empty())
            {
                return;
            }
            lifetime->GenericAdvancedInfoText().Text(std::move(info));
            lifetime->GenericAdvancedInfoScroller().Visibility(Visibility::Visible);
        }));
    }

    void MainWindow::present_text(const glance::app::PreviewFile& file, bool markdown)
    {
        current_kind_ = markdown ? glance::app::PreviewKind::markdown : glance::app::PreviewKind::text;
        show_content_panel(markdown ? glance::app::PreviewKind::markdown : glance::app::PreviewKind::text);
        current_text_.clear();
        current_text_path_ = file.path;
        current_text_markdown_ = markdown;
        current_text_encoding_ = glance::app::TextEncoding::automatic;
        markdown_preview_ = markdown;
        EncodingSelector().Content(box_value(glance::app::localize(L"EncodingDetecting")));
        apply_text_preferences();
        current_text_ = glance::app::localize(L"Loading");
        render_text_content();
        LineNumberText().Text(L"");
        MarkdownModeButtons().Visibility(markdown ? Visibility::Visible : Visibility::Collapsed);
        set_markdown_preview_mode(markdown);
        if (markdown)
        {
            MarkdownPreviewWebView().Opacity(0.0);
        }
        load_text_async(file.path, markdown, content_generation_, current_text_encoding_);
    }

    fire_and_forget MainWindow::load_text_async(
        std::wstring path,
        bool markdown,
        std::uint64_t generation,
        glance::app::TextEncoding encoding)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        co_await resume_background();
        auto preview = glance::app::load_text_preview(path, 8U * 1024U * 1024U, encoding);
        static_cast<void>(dispatcher.TryEnqueue(
            [lifetime, preview = std::move(preview), markdown, generation]() mutable {
                lifetime->apply_text_preview(std::move(preview), markdown, generation);
            }));
    }

    fire_and_forget MainWindow::load_image_async(std::wstring path, std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        try
        {
            const auto file = co_await Windows::Storage::StorageFile::GetFileFromPathAsync(path);
            const auto properties = co_await file.Properties().GetImagePropertiesAsync();
            const auto stream = co_await file.OpenReadAsync();
            Microsoft::UI::Xaml::Media::Imaging::BitmapImage bitmap;
            co_await bitmap.SetSourceAsync(stream);
            const auto width = properties.Width() != 0 ? properties.Width() : static_cast<std::uint32_t>(bitmap.PixelWidth());
            const auto height = properties.Height() != 0 ? properties.Height() : static_cast<std::uint32_t>(bitmap.PixelHeight());
            static_cast<void>(dispatcher.TryEnqueue([lifetime, bitmap, generation, width, height] {
                if (generation != lifetime->content_generation_)
                {
                    return;
                }
                lifetime->ImagePreview().Source(bitmap);
                lifetime->update_image_fit_surface();
                if (lifetime->current_index_ < lifetime->files_.size())
                {
                    const auto& current = lifetime->files_[lifetime->current_index_];
                    lifetime->FooterMetadataText().Text(
                        lifetime->formatted_size(current.size)
                        + L"  |  " + lifetime->formatted_time(current.last_write_time)
                        + L"  |  " + std::to_wstring(width) + L" x " + std::to_wstring(height));
                }
                lifetime->auto_fit_window_to_content(width, height);
            }));
        }
        catch (const hresult_error& error)
        {
            const auto message = glance::app::localize_format(L"ImageDecodeError", { error.message() });
            static_cast<void>(dispatcher.TryEnqueue([lifetime, message, generation] {
                lifetime->show_provider_error(message, generation);
            }));
        }
    }

    fire_and_forget MainWindow::load_image_metadata_async(std::wstring path, std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        co_await resume_background();
        auto metadata = glance::app::load_image_metadata(path);
        static_cast<void>(dispatcher.TryEnqueue(
            [lifetime, metadata = std::move(metadata), generation]() mutable {
                if (generation != lifetime->content_generation_)
                {
                    return;
                }
                lifetime->image_metadata_ = std::move(metadata);
                lifetime->ImageMetadataText().Text(
                    lifetime->image_metadata_.empty()
                        ? glance::app::localize(L"NoImageMetadata")
                        : lifetime->image_metadata_);
                lifetime->update_image_metadata_visibility();
            }));
    }

    fire_and_forget MainWindow::load_media_async(std::wstring path, std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        try
        {
            const auto file = co_await Windows::Storage::StorageFile::GetFileFromPathAsync(path);
            const auto source = Windows::Media::Core::MediaSource::CreateFromStorageFile(file);
            if (generation != content_generation_)
            {
                co_return;
            }

            media_is_audio_ = is_audio_path(path);
            AudioMetadataPanel().Visibility(media_is_audio_ ? Visibility::Visible : Visibility::Collapsed);
            MediaPreview().Visibility(media_is_audio_ ? Visibility::Collapsed : Visibility::Visible);
            if (media_is_audio_)
            {
                MediaPanel().Background(Application::Current().Resources().Lookup(
                    box_value(L"SolidBackgroundFillColorBaseBrush")).as<Media::Brush>());
                MediaControlsOverlay().Background(Application::Current().Resources().Lookup(
                    box_value(L"LayerFillColorDefaultBrush")).as<Media::Brush>());
                MediaPlayPauseIcon().ClearValue(IconElement::ForegroundProperty());
                MediaMuteIcon().ClearValue(IconElement::ForegroundProperty());
                MediaTimeText().ClearValue(TextBlock::ForegroundProperty());
            }
            else
            {
                MediaPanel().Background(Media::SolidColorBrush(Windows::UI::Color{ 255, 0, 0, 0 }));
                MediaControlsOverlay().Background(Media::SolidColorBrush(Windows::UI::Color{ 153, 0, 0, 0 }));
                const auto white = Media::SolidColorBrush(Windows::UI::Color{ 255, 255, 255, 255 });
                MediaPlayPauseIcon().Foreground(white);
                MediaMuteIcon().Foreground(white);
                MediaTimeText().Foreground(white);
            }
            MediaPreview().Source(source);
            MediaPreview().MediaPlayer().Volume(MediaVolumeSlider().Value() / 100.0);
            MediaPreview().MediaPlayer().Play();
            media_timer_.Start();

            std::uint64_t native_bitrate{};
            if (media_is_audio_)
            {
                try
                {
                    const auto properties = co_await file.Properties().GetMusicPropertiesAsync();
                    if (generation != content_generation_)
                    {
                        co_return;
                    }
                    const std::wstring title = properties.Title().empty()
                        ? std::filesystem::path(path).stem().wstring()
                        : std::wstring(properties.Title());
                    std::wstring artist(properties.Artist());
                    if (artist.empty())
                    {
                        artist = std::wstring(properties.AlbumArtist());
                    }
                    native_bitrate = properties.Bitrate();
                    MediaTitleText().Text(title);
                    MediaAlbumText().Text(properties.Album());
                    MediaArtistText().Text(artist);

                    const auto thumbnail = co_await file.GetThumbnailAsync(
                        Windows::Storage::FileProperties::ThumbnailMode::MusicView,
                        320);
                    if (generation == content_generation_ && thumbnail != nullptr && thumbnail.Size() > 0)
                    {
                        Microsoft::UI::Xaml::Media::Imaging::BitmapImage bitmap;
                        co_await bitmap.SetSourceAsync(thumbnail);
                        if (generation == content_generation_)
                        {
                            MediaCoverImage().Source(bitmap);
                            MediaCoverImage().Visibility(Visibility::Visible);
                            MediaCoverPlaceholder().Visibility(Visibility::Collapsed);
                        }
                    }
                }
                catch (const hresult_error&)
                {
                    MediaTitleText().Text(std::filesystem::path(path).stem().wstring());
                }
            }
            else
            {
                try
                {
                    const auto properties = co_await file.Properties().GetVideoPropertiesAsync();
                    if (generation != content_generation_ || current_index_ >= files_.size())
                    {
                        co_return;
                    }
                    native_bitrate = properties.Bitrate();
                    if (properties.Width() > 0 && properties.Height() > 0)
                    {
                        media_dimensions_ = std::to_wstring(properties.Width())
                            + L" x " + std::to_wstring(properties.Height());
                        auto_fit_window_to_content(properties.Width(), properties.Height());
                    }
                    update_media_footer();
                }
                catch (const hresult_error&)
                {
                }
            }
            load_media_technical_metadata_async(path, generation, media_is_audio_, native_bitrate);
        }
        catch (const hresult_error& error)
        {
            const auto message = glance::app::localize_format(L"MediaOpenError", { error.message() });
            lifetime->show_provider_error(message, generation);
        }
    }

    fire_and_forget MainWindow::load_media_technical_metadata_async(
        std::wstring path,
        std::uint64_t generation,
        bool audio,
        std::uint64_t fallback_bitrate)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        co_await resume_background();
        auto metadata = glance::app::probe_media_metadata(path, audio);
        if (metadata.bitrate == 0)
        {
            metadata.bitrate = fallback_bitrate;
        }
        auto formatted = glance::app::format_media_metadata(metadata, audio);
        static_cast<void>(dispatcher.TryEnqueue(
            [lifetime, generation, formatted = std::move(formatted)]() mutable {
                if (generation != lifetime->content_generation_)
                {
                    return;
                }
                lifetime->media_technical_info_ = std::move(formatted);
                lifetime->update_media_footer();
            }));
    }

    void MainWindow::update_media_footer()
    {
        if (current_index_ >= files_.size())
        {
            return;
        }
        const auto& file = files_[current_index_];
        std::wstring metadata = formatted_size(file.size)
            + L"  |  " + formatted_time(file.last_write_time);
        if (!media_dimensions_.empty())
        {
            metadata += L"  |  " + media_dimensions_;
        }
        if (!media_technical_info_.empty())
        {
            metadata += L"  |  " + media_technical_info_;
        }
        FooterMetadataText().Text(metadata);
    }

    fire_and_forget MainWindow::load_pdf_async(std::wstring path, std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        try
        {
            const auto file = co_await Windows::Storage::StorageFile::GetFileFromPathAsync(path);
            const auto document = co_await Windows::Data::Pdf::PdfDocument::LoadFromFileAsync(file);
            if (generation != content_generation_)
            {
                co_return;
            }
            pdf_document_ = document;
            pdf_page_index_ = 0;
            if (document.PageCount() == 0)
            {
                show_provider_error(glance::app::localize(L"PdfEmptyError"), generation);
                co_return;
            }
            render_pdf_page_async(pdf_page_index_, generation);
        }
        catch (const hresult_error&)
        {
            show_provider_error(
                glance::app::localize(L"PdfOpenError"),
                generation);
        }
    }

    fire_and_forget MainWindow::render_pdf_page_async(
        std::uint32_t page_index,
        std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        try
        {
            if (pdf_document_ == nullptr || page_index >= pdf_document_.PageCount())
            {
                co_return;
            }
            const auto page = pdf_document_.GetPage(page_index);
            const auto dimensions = page.Dimensions().CropBox();
            Windows::Storage::Streams::InMemoryRandomAccessStream stream;
            co_await page.RenderToStreamAsync(stream);
            stream.Seek(0);
            Microsoft::UI::Xaml::Media::Imaging::BitmapImage bitmap;
            co_await bitmap.SetSourceAsync(stream);
            if (generation != content_generation_ || page_index != pdf_page_index_)
            {
                co_return;
            }
            PdfPageImage().Source(bitmap);
            PdfLoadingOverlay().Visibility(Visibility::Collapsed);
            PdfPageText().Text(
                std::to_wstring(page_index + 1) + L" / " + std::to_wstring(pdf_document_.PageCount()));
            auto_fit_window_to_content(dimensions.Width, dimensions.Height);
        }
        catch (const hresult_error& error)
        {
            show_provider_error(
                glance::app::localize_format(L"PdfRenderError", { error.message() }),
                generation);
        }
    }

    fire_and_forget MainWindow::load_archive_async(std::wstring path, std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        co_await resume_background();
        auto preview = glance::app::load_archive_preview(path);
        static_cast<void>(dispatcher.TryEnqueue(
            [lifetime, preview = std::move(preview), generation]() mutable {
                lifetime->apply_archive_preview(std::move(preview), generation);
            }));
    }

    fire_and_forget MainWindow::load_directory_async(std::wstring path, std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        co_await resume_background();
        auto preview = glance::app::load_directory_preview(path);
        static_cast<void>(dispatcher.TryEnqueue(
            [lifetime, preview = std::move(preview), generation]() mutable {
                lifetime->apply_archive_preview(std::move(preview), generation);
            }));
    }

    void MainWindow::apply_archive_preview(
        glance::app::ArchivePreview preview,
        std::uint64_t generation)
    {
        if (generation != content_generation_)
        {
            return;
        }
        if (!preview.error.empty())
        {
            show_provider_error(std::move(preview.error), generation);
            return;
        }

        auto items = ArchiveEntryList().Items();
        items.Clear();
        for (const auto& entry : preview.entries)
        {
            Grid row;
            ColumnDefinition icon_column;
            icon_column.Width(GridLength{ 28, GridUnitType::Pixel });
            row.ColumnDefinitions().Append(icon_column);
            ColumnDefinition name_column;
            name_column.Width(GridLength{ 1, GridUnitType::Star });
            row.ColumnDefinitions().Append(name_column);
            for (const double width : { 110.0, 150.0, 90.0 })
            {
                ColumnDefinition column;
                column.Width(GridLength{ width, GridUnitType::Pixel });
                row.ColumnDefinitions().Append(column);
            }

            FontIcon icon;
            icon.FontSize(13);
            icon.Glyph(entry.is_folder ? L"\xE8B7" : L"\xE8A5");
            icon.HorizontalAlignment(HorizontalAlignment::Left);
            Grid::SetColumn(icon, 0);
            row.Children().Append(icon);

            const auto append_text = [&row](std::wstring_view value, int column, TextAlignment alignment = TextAlignment::Left) {
                TextBlock text;
                text.Text(value);
                text.FontSize(12);
                text.VerticalAlignment(VerticalAlignment::Center);
                text.TextAlignment(alignment);
                text.TextTrimming(TextTrimming::CharacterEllipsis);
                Grid::SetColumn(text, column);
                row.Children().Append(text);
            };
            append_text(entry.name, 1);
            append_text(entry.type_name, 2);
            append_text(entry.modified_time == 0 ? L"" : formatted_time(entry.modified_time), 3);
            append_text(entry.is_folder ? L"" : formatted_size(entry.size), 4, TextAlignment::Right);

            ListViewItem item;
            item.Content(row);
            items.Append(item);
        }
        std::wstring status = glance::app::localize_format(
            L"EntryCount",
            { std::to_wstring(preview.entries.size()) });
        if (preview.show_total_size)
        {
            status += L"  |  " + glance::app::localize_format(
                preview.truncated ? L"PartialTotalSize" : L"TotalSize",
                { formatted_size(preview.total_size) });
        }
        if (preview.truncated)
        {
            status += L"  |  " + glance::app::localize(L"ListTruncated");
        }
        ArchiveStatusText().Text(status);
    }

    fire_and_forget MainWindow::load_office_async(
        std::wstring path,
        std::uint64_t generation,
        std::uint64_t source_size,
        std::uint64_t source_modified_time)
    {
        std::error_code cache_error;
        if (path == office_cache_source_path_ &&
            source_size == office_cache_source_size_ &&
            source_modified_time == office_cache_source_modified_time_ &&
            std::filesystem::is_regular_file(office_temp_pdf_, cache_error))
        {
            PdfLoadingText().Text(glance::app::localize(L"LoadingConvertedDocument"));
            load_pdf_async(office_temp_pdf_, generation);
            co_return;
        }

        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        co_await resume_background();

        std::error_code filesystem_error;
        const auto cache_directory = std::filesystem::temp_directory_path(filesystem_error) / L"Glance" / L"Office";
        std::filesystem::create_directories(cache_directory, filesystem_error);
        const auto output_path = cache_directory /
            (L"preview-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(instance_id_) + L"-" + std::to_wstring(generation) + L".pdf");
        const auto staged_input_path = cache_directory /
            (L"source-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(instance_id_) + L"-" + std::to_wstring(generation) +
             std::filesystem::path(path).extension().wstring());
        const auto host_path = executable_directory() / L"Glance.OfficeHost.exe";

        DWORD exit_code = ERROR_FILE_NOT_FOUND;
        const bool staged = !filesystem_error && std::filesystem::copy_file(
            path,
            staged_input_path,
            std::filesystem::copy_options::overwrite_existing,
            filesystem_error);
        if (staged && std::filesystem::exists(host_path))
        {
            std::wstring command_line = quote_command_line_argument(host_path.wstring()) + L" " +
                quote_command_line_argument(staged_input_path.wstring()) + L" " +
                quote_command_line_argument(output_path.wstring());
            STARTUPINFOW startup{ sizeof(STARTUPINFOW) };
            PROCESS_INFORMATION process{};
            if (CreateProcessW(
                    host_path.c_str(),
                    command_line.data(),
                    nullptr,
                    nullptr,
                    FALSE,
                    CREATE_NO_WINDOW,
                    nullptr,
                    nullptr,
                    &startup,
                    &process))
            {
                CloseHandle(process.hThread);
                const DWORD wait_result = WaitForSingleObject(process.hProcess, 120000);
                if (wait_result == WAIT_TIMEOUT)
                {
                    TerminateProcess(process.hProcess, ERROR_TIMEOUT);
                    WaitForSingleObject(process.hProcess, 5000);
                }
                static_cast<void>(GetExitCodeProcess(process.hProcess, &exit_code));
                CloseHandle(process.hProcess);
            }
        }
        DeleteFileW(staged_input_path.c_str());

        filesystem_error.clear();
        const bool succeeded = exit_code == 0 && std::filesystem::is_regular_file(output_path, filesystem_error);
        static_cast<void>(dispatcher.TryEnqueue([
            lifetime,
            source = std::move(path),
            source_size,
            source_modified_time,
            output = output_path.wstring(),
            generation,
            succeeded] {
            if (!succeeded)
            {
                DeleteFileW(output.c_str());
                if (generation == lifetime->content_generation_)
                {
                    lifetime->show_provider_error(
                        glance::app::localize(L"OfficeConvertError"),
                        generation);
                }
                return;
            }
            if (!lifetime->office_temp_pdf_.empty() && lifetime->office_temp_pdf_ != output)
            {
                DeleteFileW(lifetime->office_temp_pdf_.c_str());
            }
            lifetime->office_temp_pdf_ = output;
            lifetime->office_cache_source_path_ = source;
            lifetime->office_cache_source_size_ = source_size;
            lifetime->office_cache_source_modified_time_ = source_modified_time;
            if (generation != lifetime->content_generation_)
            {
                return;
            }
            lifetime->PdfPageText().Text(L"1 / 1");
            lifetime->load_pdf_async(output, generation);
        }));
    }

    void MainWindow::apply_text_preview(
        glance::app::TextPreview preview,
        bool markdown,
        std::uint64_t generation)
    {
        if (generation != content_generation_)
        {
            return;
        }
        if (!preview.error.empty())
        {
            if (current_text_encoding_ == glance::app::TextEncoding::automatic)
            {
                EncodingSelector().Content(box_value(glance::app::localize(L"EncodingUnknown")));
            }
            current_text_ = std::move(preview.error);
            render_text_content();
            LineNumberText().Text(L"");
            TextPreviewScroller().Visibility(Visibility::Visible);
            MarkdownPreviewWebView().Visibility(Visibility::Collapsed);
            return;
        }

        current_text_ = std::move(preview.content);
        if (current_text_encoding_ == glance::app::TextEncoding::automatic)
        {
            EncodingSelector().Content(box_value(preview.encoding));
        }
        TextEncodingText().Text(preview.truncated ? glance::app::localize(L"PreviewTruncated") : L"");

        render_text_content();
        update_line_numbers();
        update_line_number_visibility();

        if (markdown)
        {
            render_markdown();
            set_markdown_preview_mode(markdown_preview_);
        }
    }

    void MainWindow::render_markdown()
    {
        const auto html = glance::app::render_markdown_html(
            current_text_,
            RootGrid().ActualTheme() == ElementTheme::Dark);
        const auto generation = content_generation_;
        const auto weak = get_weak();
        static_cast<void>(DispatcherQueue().TryEnqueue(
            [weak, html, generation] {
                if (const auto self = weak.get();
                    self != nullptr && generation == self->content_generation_)
                {
                    self->render_markdown_async(html, generation);
                }
            }));
    }

    fire_and_forget MainWindow::render_markdown_async(std::wstring html, std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        try
        {
            co_await MarkdownPreviewWebView().EnsureCoreWebView2Async();
            if (generation != content_generation_)
            {
                co_return;
            }
            MarkdownPreviewWebView().NavigateToString(html);
            MarkdownPreviewWebView().Opacity(1.0);
        }
        catch (const hresult_error& error)
        {
            glance::contracts::log_event(
                L"Markdown WebView initialization failed: " + std::wstring(error.message()));
            if (generation == content_generation_)
            {
                set_markdown_preview_mode(false);
            }
        }
    }

    void MainWindow::render_text_content()
    {
        auto blocks = TextContentRichText().Blocks();
        blocks.Clear();
        Paragraph paragraph;
        const double line_height = std::max(18.0, std::ceil(text_preferences_.font_size * 1.38));
        paragraph.LineHeight(line_height);
        paragraph.LineStackingStrategy(LineStackingStrategy::BlockLineHeight);
        LineNumberText().LineHeight(line_height);

        const bool use_highlighting = syntax_highlighting_ && current_text_.size() <= 1024U * 1024U;
        if (use_highlighting)
        {
            auto extension = std::filesystem::path(current_text_path_).extension().wstring();
            std::ranges::transform(extension, extension.begin(), [](wchar_t value) {
                return static_cast<wchar_t>(std::towlower(value));
            });
            const bool dark = RootGrid().ActualTheme() == ElementTheme::Dark;
            for (const auto& span : glance::app::highlight_source(current_text_, extension))
            {
                Run run;
                run.Text(span.text);
                if (const auto foreground = syntax_brush(span.style, text_preferences_.syntax_theme, dark))
                {
                    run.Foreground(foreground);
                }
                paragraph.Inlines().Append(run);
            }
        }
        else
        {
            Run run;
            run.Text(current_text_.empty() ? L" " : current_text_);
            paragraph.Inlines().Append(run);
        }
        blocks.Append(paragraph);
    }

    void MainWindow::apply_text_preferences()
    {
        text_preferences_ = glance::app::load_text_preferences();
        line_numbers_visible_ = text_preferences_.line_numbers;
        syntax_highlighting_ = text_preferences_.syntax_highlighting;
        word_wrap_ = text_preferences_.word_wrap;
        const Media::FontFamily font(text_preferences_.font_family);
        TextContentRichText().FontFamily(font);
        LineNumberText().FontFamily(font);
        apply_text_font_metrics();
        SyntaxHighlightButton().IsChecked(syntax_highlighting_);
        WordWrapButton().IsChecked(word_wrap_);
        update_text_layout();
        update_line_number_visibility();
    }

    void MainWindow::apply_text_font_metrics()
    {
        TextContentRichText().FontSize(text_preferences_.font_size);
        LineNumberText().FontSize(text_preferences_.font_size);
        const double line_height = std::max(18.0, std::ceil(text_preferences_.font_size * 1.38));
        LineNumberText().LineHeight(line_height);
        const auto blocks = TextContentRichText().Blocks();
        if (blocks.Size() > 0)
        {
            if (const auto paragraph = blocks.GetAt(0).try_as<Paragraph>())
            {
                paragraph.LineHeight(line_height);
                paragraph.LineStackingStrategy(LineStackingStrategy::BlockLineHeight);
            }
        }
    }

    void MainWindow::update_text_layout()
    {
        TextContentRichText().TextWrapping(word_wrap_ ? TextWrapping::Wrap : TextWrapping::NoWrap);
        TextPreviewScroller().HorizontalScrollMode(
            word_wrap_ ? ScrollMode::Disabled : ScrollMode::Enabled);
        TextPreviewScroller().HorizontalScrollBarVisibility(
            word_wrap_ ? ScrollBarVisibility::Disabled : ScrollBarVisibility::Auto);
        TextContentRichText().Width(
            word_wrap_
                ? std::max(1.0, TextPreviewScroller().ActualWidth() - LineNumberGutter().ActualWidth())
                : std::numeric_limits<double>::quiet_NaN());
        update_line_numbers();
        update_line_number_visibility();
    }

    void MainWindow::update_line_numbers()
    {
        if (current_text_.empty())
        {
            LineNumberText().Text(L"");
            return;
        }

        const auto simple_numbers = [this] {
            const std::size_t line_count = 1
                + static_cast<std::size_t>(std::ranges::count(current_text_, L'\n'));
            std::wostringstream output;
            for (std::size_t line = 1; line <= line_count; ++line)
            {
                if (line > 1)
                {
                    output << L'\n';
                }
                output << line;
            }
            return output.str();
        };

        if (!word_wrap_ || current_text_.size() > 1024U * 1024U)
        {
            LineNumberText().Text(simple_numbers());
            return;
        }

        const double content_width = TextContentRichText().Width() - 32.0;
        if (!std::isfinite(content_width) || content_width <= 1.0)
        {
            LineNumberText().Text(simple_numbers());
            return;
        }

        com_ptr<IDWriteFactory> factory;
        if (FAILED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(factory.put()))))
        {
            LineNumberText().Text(simple_numbers());
            return;
        }
        com_ptr<IDWriteTextFormat> format;
        if (FAILED(factory->CreateTextFormat(
                text_preferences_.font_family.c_str(),
                nullptr,
                DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                static_cast<float>(text_preferences_.font_size),
                L"",
                format.put())))
        {
            LineNumberText().Text(simple_numbers());
            return;
        }
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        com_ptr<IDWriteTextLayout> layout;
        if (FAILED(factory->CreateTextLayout(
                current_text_.data(),
                static_cast<UINT32>(current_text_.size()),
                format.get(),
                static_cast<float>(content_width),
                std::numeric_limits<float>::max(),
                layout.put())))
        {
            LineNumberText().Text(simple_numbers());
            return;
        }

        UINT32 metric_count{};
        layout->GetLineMetrics(nullptr, 0, &metric_count);
        std::vector<DWRITE_LINE_METRICS> metrics(metric_count);
        if (FAILED(layout->GetLineMetrics(metrics.data(), metric_count, &metric_count)))
        {
            LineNumberText().Text(simple_numbers());
            return;
        }

        std::wostringstream output;
        std::size_t logical_line = 1;
        bool first_visual_line = true;
        for (UINT32 index = 0; index < metric_count; ++index)
        {
            if (index > 0)
            {
                output << L'\n';
            }
            if (first_visual_line)
            {
                output << logical_line;
            }
            first_visual_line = false;
            if (metrics[index].newlineLength > 0)
            {
                ++logical_line;
                first_visual_line = true;
            }
        }
        if (!current_text_.empty() && current_text_.back() == L'\n')
        {
            output << L'\n' << logical_line;
        }
        LineNumberText().Text(output.str());
    }

    void MainWindow::show_content_panel(glance::app::PreviewKind kind)
    {
        const bool text = kind == glance::app::PreviewKind::text || kind == glance::app::PreviewKind::markdown;
        GenericPanel().Visibility(kind == glance::app::PreviewKind::generic ? Visibility::Visible : Visibility::Collapsed);
        TextPanel().Visibility(text ? Visibility::Visible : Visibility::Collapsed);
        ImagePanel().Visibility(kind == glance::app::PreviewKind::image ? Visibility::Visible : Visibility::Collapsed);
        MediaPanel().Visibility(kind == glance::app::PreviewKind::media ? Visibility::Visible : Visibility::Collapsed);
        PdfPanel().Visibility(kind == glance::app::PreviewKind::pdf ? Visibility::Visible : Visibility::Collapsed);
        ArchivePanel().Visibility(kind == glance::app::PreviewKind::archive ? Visibility::Visible : Visibility::Collapsed);
        ImageStatusControls().Visibility(
            kind == glance::app::PreviewKind::image ? Visibility::Visible : Visibility::Collapsed);
        TextStatusControls().Visibility(text ? Visibility::Visible : Visibility::Collapsed);
        LineNumbersButton().Visibility(text ? Visibility::Visible : Visibility::Collapsed);
        SyntaxHighlightButton().Visibility(text ? Visibility::Visible : Visibility::Collapsed);
        WordWrapButton().Visibility(text ? Visibility::Visible : Visibility::Collapsed);
        if (kind != glance::app::PreviewKind::image)
        {
            ImagePreview().Source(nullptr);
            image_metadata_.clear();
            image_metadata_visible_ = false;
            ImageExifButton().IsChecked(false);
            ImageMetadataOverlay().Visibility(Visibility::Collapsed);
        }
        if (kind != glance::app::PreviewKind::media)
        {
            stop_media_playback();
        }
        if (kind != glance::app::PreviewKind::pdf)
        {
            pdf_document_ = nullptr;
            PdfPageImage().Source(nullptr);
        }
    }

    void MainWindow::show_provider_error(std::wstring message, std::uint64_t generation)
    {
        if (generation != content_generation_ || current_index_ >= files_.size())
        {
            return;
        }
        present_generic(files_[current_index_]);
        ErrorText().Text(std::move(message));
        ErrorText().Visibility(Visibility::Visible);
    }

    void MainWindow::update_image_fit_surface()
    {
        ImageFitSurface().Width(std::max(1.0, ImagePanel().ActualWidth()));
        ImageFitSurface().Height(std::max(1.0, ImagePanel().ActualHeight()));
    }

    void MainWindow::update_pdf_fit_surface()
    {
        PdfFitSurface().Width(std::max(1.0, PdfScroller().ActualWidth()));
        PdfFitSurface().Height(std::max(1.0, PdfScroller().ActualHeight()));
    }

    void MainWindow::TopmostButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        topmost_ = TopmostButton().IsChecked().Value();
        set_topmost(topmost_);
        update_state();
    }

    void MainWindow::ClosePreviewButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (detached_)
        {
            stop_detached_focus_monitor();
            clear_preview_content();
            Close();
            return;
        }
        pinned_ = false;
        topmost_ = false;
        PinButton().IsChecked(false);
        TopmostButton().IsChecked(false);
        set_topmost(false);
        HidePreview();
    }

    void MainWindow::set_markdown_preview_mode(bool preview)
    {
        markdown_preview_ = preview;
        MarkdownPreviewButton().IsChecked(preview);
        MarkdownCodeButton().IsChecked(!preview);
        MarkdownPreviewButton().FontWeight(preview ? Windows::UI::Text::FontWeights::SemiBold() : Windows::UI::Text::FontWeights::Normal());
        MarkdownCodeButton().FontWeight(preview ? Windows::UI::Text::FontWeights::Normal() : Windows::UI::Text::FontWeights::SemiBold());
        TextPreviewScroller().Visibility(preview ? Visibility::Collapsed : Visibility::Visible);
        MarkdownPreviewWebView().Visibility(preview ? Visibility::Visible : Visibility::Collapsed);
    }

    void MainWindow::MarkdownPreviewButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        set_markdown_preview_mode(true);
    }

    void MainWindow::MarkdownCodeButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        set_markdown_preview_mode(false);
    }

    void MainWindow::update_line_number_visibility()
    {
        LineNumberGutter().Visibility(line_numbers_visible_ ? Visibility::Visible : Visibility::Collapsed);
        LineNumbersButton().IsEnabled(true);
        LineNumbersButton().IsChecked(line_numbers_visible_);
        ToolTipService::SetToolTip(
            LineNumbersButton(),
            box_value(glance::app::localize(L"LineNumbersTooltip")));
    }

    void MainWindow::LineNumbersButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        line_numbers_visible_ = LineNumbersButton().IsChecked().Value();
        text_preferences_.line_numbers = line_numbers_visible_;
        glance::app::save_text_preferences(text_preferences_);
        update_line_numbers();
        update_line_number_visibility();
    }

    void MainWindow::SyntaxHighlightButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        syntax_highlighting_ = SyntaxHighlightButton().IsChecked().Value();
        text_preferences_.syntax_highlighting = syntax_highlighting_;
        glance::app::save_text_preferences(text_preferences_);
        render_text_content();
    }

    void MainWindow::WordWrapButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        word_wrap_ = WordWrapButton().IsChecked().Value();
        text_preferences_.word_wrap = word_wrap_;
        glance::app::save_text_preferences(text_preferences_);
        update_text_layout();
    }

    void MainWindow::EncodingOption_Click(IInspectable const& sender, RoutedEventArgs const&)
    {
        const auto option = sender.try_as<MenuFlyoutItem>();
        if (!option || current_text_path_.empty())
        {
            return;
        }

        const auto tag = unbox_value_or<hstring>(option.Tag(), L"");
        if (tag == L"utf8")
        {
            current_text_encoding_ = glance::app::TextEncoding::utf8;
        }
        else if (tag == L"utf16_le")
        {
            current_text_encoding_ = glance::app::TextEncoding::utf16_le;
        }
        else if (tag == L"utf16_be")
        {
            current_text_encoding_ = glance::app::TextEncoding::utf16_be;
        }
        else if (tag == L"gb2312")
        {
            current_text_encoding_ = glance::app::TextEncoding::gb2312;
        }
        else if (tag == L"gbk")
        {
            current_text_encoding_ = glance::app::TextEncoding::gbk;
        }
        else if (tag == L"gb18030")
        {
            current_text_encoding_ = glance::app::TextEncoding::gb18030;
        }
        else if (tag == L"big5")
        {
            current_text_encoding_ = glance::app::TextEncoding::big5;
        }
        else if (tag == L"system")
        {
            current_text_encoding_ = glance::app::TextEncoding::system;
        }
        else
        {
            return;
        }

        EncodingSelector().Content(box_value(option.Text()));
        current_text_ = glance::app::localize(L"Loading");
        render_text_content();
        LineNumberText().Text(L"");
        const auto generation = ++content_generation_;
        load_text_async(current_text_path_, current_text_markdown_, generation, current_text_encoding_);
    }

    void MainWindow::TextPreviewScroller_SizeChanged(IInspectable const&, SizeChangedEventArgs const&)
    {
        if (word_wrap_)
        {
            update_text_layout();
        }
    }

    void MainWindow::TextPreviewScroller_PointerWheelChanged(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        if ((GetKeyState(VK_CONTROL) & 0x8000) == 0)
        {
            text_font_wheel_delta_ = 0;
            return;
        }

        args.Handled(true);
        text_font_wheel_delta_ += args.GetCurrentPoint(TextCodePanel()).Properties().MouseWheelDelta();
        const int steps = text_font_wheel_delta_ / WHEEL_DELTA;
        text_font_wheel_delta_ %= WHEEL_DELTA;
        if (steps == 0)
        {
            return;
        }

        const double font_size = std::clamp(text_preferences_.font_size + steps, 9.0, 32.0);
        if (font_size != text_preferences_.font_size)
        {
            text_preferences_.font_size = font_size;
            glance::app::save_text_preferences(text_preferences_);
            apply_text_font_metrics();
            update_text_layout();
        }
        show_text_font_size_overlay();
    }

    void MainWindow::show_text_font_size_overlay()
    {
        TextFontSizeOverlayText().Text(glance::app::localize_format(
            L"FontSizeOverlayFormat",
            { std::to_wstring(static_cast<int>(text_preferences_.font_size)) }));
        TextFontSizeOverlay().Visibility(Visibility::Visible);

        if (font_size_overlay_timer_ == nullptr)
        {
            font_size_overlay_timer_ = DispatcherTimer();
            font_size_overlay_timer_.Interval(std::chrono::milliseconds(900));
            const auto weak = get_weak();
            font_size_overlay_timer_.Tick([weak](IInspectable const&, IInspectable const&) {
                if (const auto self = weak.get())
                {
                    self->font_size_overlay_timer_.Stop();
                    self->TextFontSizeOverlay().Visibility(Visibility::Collapsed);
                }
            });
        }
        font_size_overlay_timer_.Stop();
        font_size_overlay_timer_.Start();
    }

    void MainWindow::ImagePanel_SizeChanged(IInspectable const&, SizeChangedEventArgs const&)
    {
        update_image_fit_surface();
    }

    void MainWindow::PdfPanel_SizeChanged(IInspectable const&, SizeChangedEventArgs const&)
    {
        update_pdf_fit_surface();
    }

    void MainWindow::update_image_metadata_visibility()
    {
        const bool show = image_metadata_visible_ && !ImageMetadataText().Text().empty();
        ImageMetadataOverlay().Visibility(show ? Visibility::Visible : Visibility::Collapsed);
        ImageExifButton().IsChecked(image_metadata_visible_);
    }

    void MainWindow::ImageExifButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        image_metadata_visible_ = ImageExifButton().IsChecked().Value();
        update_image_metadata_visibility();
    }

    void MainWindow::set_image_zoom(float zoom, Windows::Foundation::Point anchor)
    {
        const float old_zoom = ImageScroller().ZoomFactor();
        const float new_zoom = std::clamp(zoom, 1.0F, 16.0F);
        if (std::abs(new_zoom - old_zoom) < 0.001F)
        {
            return;
        }

        const double horizontal =
            (ImageScroller().HorizontalOffset() + anchor.X) * new_zoom / old_zoom - anchor.X;
        const double vertical =
            (ImageScroller().VerticalOffset() + anchor.Y) * new_zoom / old_zoom - anchor.Y;
        static_cast<void>(ImageScroller().ChangeView(
            std::max(0.0, horizontal),
            std::max(0.0, vertical),
            new_zoom,
            true));
    }

    void MainWindow::ZoomOutButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        const float zoom = std::max(1.0F, ImageScroller().ZoomFactor() / 1.25F);
        set_image_zoom(
            zoom,
            Windows::Foundation::Point{
                static_cast<float>(ImageScroller().ActualWidth() / 2.0),
                static_cast<float>(ImageScroller().ActualHeight() / 2.0) });
    }

    void MainWindow::ZoomInButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        const float zoom = std::min(16.0F, ImageScroller().ZoomFactor() * 1.25F);
        set_image_zoom(
            zoom,
            Windows::Foundation::Point{
                static_cast<float>(ImageScroller().ActualWidth() / 2.0),
                static_cast<float>(ImageScroller().ActualHeight() / 2.0) });
    }

    void MainWindow::ImageScroller_PointerWheelChanged(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        const auto point = args.GetCurrentPoint(ImageScroller());
        const int delta = point.Properties().MouseWheelDelta();
        if (delta == 0)
        {
            return;
        }
        const float factor = std::pow(1.2F, static_cast<float>(delta) / 120.0F);
        set_image_zoom(ImageScroller().ZoomFactor() * factor, point.Position());
        args.Handled(true);
    }

    void MainWindow::ImageScroller_PointerPressed(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        const auto point = args.GetCurrentPoint(ImageScroller());
        if (!point.Properties().IsLeftButtonPressed() || ImageScroller().ZoomFactor() <= 1.001F)
        {
            return;
        }
        if (!ImageScroller().CapturePointer(args.Pointer()))
        {
            return;
        }
        image_panning_ = true;
        image_pan_start_ = point.Position();
        image_pan_horizontal_offset_ = ImageScroller().HorizontalOffset();
        image_pan_vertical_offset_ = ImageScroller().VerticalOffset();
        args.Handled(true);
    }

    void MainWindow::ImageScroller_PointerMoved(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        if (!image_panning_)
        {
            return;
        }
        const auto point = args.GetCurrentPoint(ImageScroller());
        if (!point.Properties().IsLeftButtonPressed())
        {
            end_image_pan(args);
            return;
        }
        const auto position = point.Position();
        static_cast<void>(ImageScroller().ChangeView(
            std::max(0.0, image_pan_horizontal_offset_ + image_pan_start_.X - position.X),
            std::max(0.0, image_pan_vertical_offset_ + image_pan_start_.Y - position.Y),
            nullptr,
            true));
        args.Handled(true);
    }

    void MainWindow::end_image_pan(PointerRoutedEventArgs const& args)
    {
        if (!image_panning_)
        {
            return;
        }
        image_panning_ = false;
        ImageScroller().ReleasePointerCapture(args.Pointer());
        args.Handled(true);
    }

    void MainWindow::ImageScroller_PointerReleased(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        end_image_pan(args);
    }

    void MainWindow::ImageScroller_PointerCanceled(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        end_image_pan(args);
    }

    void MainWindow::ImageScroller_PointerCaptureLost(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        image_panning_ = false;
        args.Handled(true);
    }

    void MainWindow::RotateButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        image_rotation_ = std::fmod(image_rotation_ + 90.0, 360.0);
        ImageTransform().Rotation(image_rotation_);
    }

    void MainWindow::show_media_controls()
    {
        media_controls_idle_ticks_ = 0;
        MediaControlsOverlay().Opacity(1.0);
        MediaControlsOverlay().IsHitTestVisible(true);
    }

    void MainWindow::stop_media_playback()
    {
        media_timer_.Stop();
        if (MediaPreview().MediaPlayer() != nullptr)
        {
            MediaPreview().MediaPlayer().Pause();
        }
        MediaPreview().Source(nullptr);
    }

    void MainWindow::update_media_controls()
    {
        if (MediaPanel().Visibility() != Visibility::Visible || MediaPreview().MediaPlayer() == nullptr)
        {
            return;
        }
        const auto player = MediaPreview().MediaPlayer();
        const auto session = player.PlaybackSession();
        const double duration = std::max(0.0, session.NaturalDuration().count() / 10000000.0);
        const double position = std::max(0.0, session.Position().count() / 10000000.0);
        updating_media_position_ = true;
        MediaSeekSlider().Maximum(std::max(1.0, duration));
        MediaSeekSlider().Value(std::min(position, std::max(1.0, duration)));
        updating_media_position_ = false;

        const auto format_duration = [](double seconds) {
            const auto total = static_cast<std::uint64_t>(seconds);
            const auto hours = total / 3600;
            const auto minutes = total / 60 % 60;
            const auto remaining = total % 60;
            std::wostringstream output;
            if (hours > 0)
            {
                output << hours << L':' << std::setfill(L'0') << std::setw(2) << minutes;
            }
            else
            {
                output << minutes;
            }
            output << L':' << std::setfill(L'0') << std::setw(2) << remaining;
            return output.str();
        };
        MediaTimeText().Text(format_duration(position) + L" / " + format_duration(duration));
        const bool playing = session.PlaybackState() == Windows::Media::Playback::MediaPlaybackState::Playing;
        MediaPlayPauseIcon().Glyph(playing ? L"\xE769" : L"\xE768");
        MediaMuteIcon().Glyph(player.IsMuted() || player.Volume() == 0.0 ? L"\xE74F" : L"\xE767");

        if (!media_is_audio_ && playing && MediaControlsOverlay().Opacity() > 0.0)
        {
            ++media_controls_idle_ticks_;
            if (media_controls_idle_ticks_ >= 10)
            {
                MediaControlsOverlay().Opacity(0.0);
                MediaControlsOverlay().IsHitTestVisible(false);
            }
        }
    }

    void MainWindow::MediaPanel_PointerMoved(IInspectable const&, PointerRoutedEventArgs const&)
    {
        show_media_controls();
    }

    void MainWindow::MediaPlayPauseButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (MediaPreview().MediaPlayer() == nullptr)
        {
            return;
        }
        const auto player = MediaPreview().MediaPlayer();
        if (player.PlaybackSession().PlaybackState() == Windows::Media::Playback::MediaPlaybackState::Playing)
        {
            player.Pause();
        }
        else
        {
            player.Play();
        }
        show_media_controls();
    }

    void MainWindow::MediaMuteButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (MediaPreview().MediaPlayer() == nullptr)
        {
            return;
        }
        const auto player = MediaPreview().MediaPlayer();
        player.IsMuted(!player.IsMuted());
        MediaMuteIcon().Glyph(player.IsMuted() ? L"\xE74F" : L"\xE767");
        show_media_controls();
    }

    void MainWindow::MediaSeekSlider_ValueChanged(
        IInspectable const&,
        Primitives::RangeBaseValueChangedEventArgs const& args)
    {
        if (!updating_media_position_ && MediaPreview().MediaPlayer() != nullptr)
        {
            MediaPreview().MediaPlayer().PlaybackSession().Position(
                std::chrono::duration_cast<Windows::Foundation::TimeSpan>(
                    std::chrono::duration<double>(args.NewValue())));
            show_media_controls();
        }
    }

    void MainWindow::MediaVolumeSlider_ValueChanged(
        IInspectable const&,
        Primitives::RangeBaseValueChangedEventArgs const& args)
    {
        if (MediaPreview().MediaPlayer() != nullptr)
        {
            MediaPreview().MediaPlayer().Volume(args.NewValue() / 100.0);
            show_media_controls();
        }
    }

    void MainWindow::PreviousPdfPageButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (pdf_document_ == nullptr || pdf_page_index_ == 0)
        {
            return;
        }
        --pdf_page_index_;
        render_pdf_page_async(pdf_page_index_, content_generation_);
    }

    void MainWindow::NextPdfPageButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (pdf_document_ == nullptr || pdf_page_index_ + 1 >= pdf_document_.PageCount())
        {
            return;
        }
        ++pdf_page_index_;
        render_pdf_page_async(pdf_page_index_, content_generation_);
    }

    void MainWindow::PdfScroller_PointerWheelChanged(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
        {
            pdf_wheel_delta_ = 0;
            return;
        }
        if (PdfScroller().ZoomFactor() > 1.001F || pdf_document_ == nullptr)
        {
            pdf_wheel_delta_ = 0;
            return;
        }

        pdf_wheel_delta_ += args.GetCurrentPoint(PdfFitSurface()).Properties().MouseWheelDelta();
        if (std::abs(pdf_wheel_delta_) >= WHEEL_DELTA)
        {
            if (pdf_wheel_delta_ > 0 && pdf_page_index_ > 0)
            {
                --pdf_page_index_;
                render_pdf_page_async(pdf_page_index_, content_generation_);
            }
            else if (pdf_wheel_delta_ < 0 && pdf_page_index_ + 1 < pdf_document_.PageCount())
            {
                ++pdf_page_index_;
                render_pdf_page_async(pdf_page_index_, content_generation_);
            }
            pdf_wheel_delta_ = 0;
        }
        args.Handled(true);
    }

    void MainWindow::PinButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        pinned_ = PinButton().IsChecked().Value();
        if (detached_ && !pinned_)
        {
            state_ = glance::contracts::PreviewWindowState::detached_unpinned;
            foreground_when_unpinned_ = GetForegroundWindow();
            start_detached_focus_monitor();
            if (state_callback_)
            {
                state_callback_(instance_id_, state_);
            }
            return;
        }
        update_state();
    }

    void MainWindow::update_state()
    {
        if (!visible_)
        {
            state_ = glance::contracts::PreviewWindowState::hidden;
        }
        else if (detached_)
        {
            state_ = pinned_
                ? glance::contracts::PreviewWindowState::detached_pinned_topmost
                : glance::contracts::PreviewWindowState::detached_unpinned;
        }
        else if (pinned_ && topmost_)
        {
            detached_ = true;
            state_ = glance::contracts::PreviewWindowState::detached_pinned_topmost;
        }
        else if (pinned_)
        {
            state_ = glance::contracts::PreviewWindowState::active_pinned;
        }
        else if (topmost_)
        {
            state_ = glance::contracts::PreviewWindowState::active_topmost;
        }
        else
        {
            state_ = glance::contracts::PreviewWindowState::active_following;
        }

        if (state_callback_)
        {
            state_callback_(instance_id_, state_);
        }
    }

    void MainWindow::set_topmost(bool enabled)
    {
        SetWindowPos(
            window_,
            enabled ? HWND_TOPMOST : HWND_NOTOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void MainWindow::start_detached_focus_monitor()
    {
        if (focus_timer_ == nullptr)
        {
            focus_timer_ = DispatcherTimer();
            focus_timer_.Interval(std::chrono::milliseconds(100));
            const auto weak = get_weak();
            focus_timer_.Tick([weak](IInspectable const&, IInspectable const&)
            {
                if (const auto self = weak.get();
                    self != nullptr && GetForegroundWindow() != self->foreground_when_unpinned_)
                {
                    self->stop_detached_focus_monitor();
                    self->Close();
                }
            });
        }
        focus_timer_.Start();
    }

    void MainWindow::stop_detached_focus_monitor()
    {
        if (focus_timer_ != nullptr)
        {
            focus_timer_.Stop();
        }
    }

    void MainWindow::FileList_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        const int index = FileList().SelectedIndex();
        if (index >= 0)
        {
            present_file(static_cast<std::uint32_t>(index));
        }
    }

    void MainWindow::CopyPathButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (current_index_ >= files_.size())
        {
            return;
        }
        try
        {
            Windows::ApplicationModel::DataTransfer::DataPackage package;
            std::wstring path = !files_[current_index_].path.empty()
                ? files_[current_index_].path
                : files_[current_index_].parsing_name;
            const auto preferences = glance::app::load_path_copy_preferences();
            if (preferences.use_unix_separators)
            {
                std::replace(path.begin(), path.end(), L'\\', L'/');
            }
            if (preferences.quote_path)
            {
                path = L"\"" + path + L"\"";
            }
            package.SetText(path);
            Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
            Windows::ApplicationModel::DataTransfer::Clipboard::Flush();

            CopyPathIcon().Glyph(L"\xE73E");
            CopyPathIcon().Foreground(Application::Current().Resources().Lookup(
                box_value(L"AccentTextFillColorPrimaryBrush")).as<Media::Brush>());
            if (copy_feedback_timer_ == nullptr)
            {
                copy_feedback_timer_ = DispatcherTimer();
                copy_feedback_timer_.Interval(std::chrono::milliseconds(1200));
                const auto weak = get_weak();
                copy_feedback_timer_.Tick([weak](IInspectable const&, IInspectable const&) {
                    if (const auto self = weak.get())
                    {
                        self->copy_feedback_timer_.Stop();
                        self->CopyPathIcon().Glyph(L"\xE8C8");
                        self->CopyPathIcon().ClearValue(IconElement::ForegroundProperty());
                    }
                });
            }
            copy_feedback_timer_.Stop();
            copy_feedback_timer_.Start();
        }
        catch (const hresult_error& error)
        {
            glance::contracts::log_event(
                L"Copy path failed: " + std::wstring(error.message()));
        }
    }

    void MainWindow::OpenFolderButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (current_index_ >= files_.size() || files_[current_index_].path.empty())
        {
            return;
        }
        std::wstring parameters = L"/select,\"" + files_[current_index_].path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", parameters.c_str(), nullptr, SW_SHOWNORMAL);
    }

    void MainWindow::OpenDefaultButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (current_index_ >= files_.size() || files_[current_index_].path.empty())
        {
            return;
        }
        ShellExecuteW(nullptr, L"open", files_[current_index_].path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void MainWindow::OpenDefaultButton_RightTapped(
        IInspectable const&,
        RightTappedRoutedEventArgs const& args)
    {
        args.Handled(true);
        if (current_index_ >= files_.size() || files_[current_index_].path.empty() ||
            (files_[current_index_].attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            return;
        }
        const OPENASINFO open_as{
            files_[current_index_].path.c_str(),
            nullptr,
            OAIF_ALLOW_REGISTRATION | OAIF_EXEC };
        static_cast<void>(SHOpenWithDialog(window_, &open_as));
    }

    void MainWindow::LoadCloudFileButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (current_index_ >= files_.size() || files_[current_index_].path.empty())
        {
            return;
        }

        const HANDLE file = CreateFileW(
            files_[current_index_].path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            ErrorText().Text(glance::app::localize_format(
                L"CloudLoadError", { std::to_wstring(GetLastError()) }));
            ErrorText().Visibility(Visibility::Visible);
            return;
        }
        CloseHandle(file);
        files_[current_index_].is_cloud_placeholder = false;
        LoadCloudFileButton().Visibility(Visibility::Collapsed);
        ErrorText().Visibility(Visibility::Collapsed);
        present_file(current_index_);
    }

    std::wstring MainWindow::formatted_size(std::uint64_t size) const
    {
        constexpr std::array<const wchar_t*, 5> units{ L"B", L"KB", L"MB", L"GB", L"TB" };
        double value = static_cast<double>(size);
        std::size_t unit{};
        while (value >= 1024.0 && unit + 1 < units.size())
        {
            value /= 1024.0;
            ++unit;
        }
        std::wostringstream output;
        output.precision(unit == 0 ? 0 : 1);
        output << std::fixed << value << L' ' << units[unit];
        return output.str();
    }

    std::wstring MainWindow::formatted_time(std::uint64_t file_time) const
    {
        FILETIME utc{ static_cast<DWORD>(file_time), static_cast<DWORD>(file_time >> 32U) };
        FILETIME local{};
        SYSTEMTIME system_time{};
        if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &system_time))
        {
            return glance::app::localize(L"UnknownTime");
        }
        wchar_t date[64]{};
        wchar_t time[64]{};
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &system_time, nullptr, date, 64, nullptr);
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &system_time, nullptr, time, 64);
        return std::wstring(date) + L" " + time;
    }
}

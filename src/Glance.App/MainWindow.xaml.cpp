#include "pch.h"
#include "MainWindow.xaml.h"
#include "appearance_preferences.h"
#include "footer_preferences.h"
#include "generic_file_info.h"
#include "image_metadata_provider.h"
#include "localization.h"
#include "markdown_renderer.h"
#include "media_metadata_provider.h"
#include "office_availability.h"
#include "path_copy_preferences.h"
#include "resource.h"
#include "shell_icon_provider.h"
#include "syntax_highlighter.h"
#include "window_size_store.h"
#include "window_preferences.h"
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
#include <optional>
#include <span>
#include <sstream>
#include <unordered_map>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Documents;
using namespace Microsoft::UI::Xaml::Input;

namespace
{
    struct __declspec(uuid("905a0fef-bc53-11df-8c49-001e4fc686da")) BufferByteAccess : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE Buffer(byte** value) = 0;
    };
    constexpr std::size_t text_chunk_bytes = 256U * 1024U;
    constexpr std::size_t long_text_render_threshold = 256U * 1024U;
    constexpr std::uint64_t automatic_syntax_highlight_limit_bytes = 64ULL * 1024ULL;
    constexpr std::uint64_t maximum_preview_as_text_bytes = 8ULL * 1024ULL * 1024ULL;

    enum class ArchiveColumnKind
    {
        name,
        type,
        modified_time,
        compressed_size,
        original_size,
    };

    struct ArchiveColumnSpec
    {
        ArchiveColumnKind kind;
        double width;
    };

    constexpr std::array folder_columns{
        ArchiveColumnSpec{ ArchiveColumnKind::name, 0.0 },
        ArchiveColumnSpec{ ArchiveColumnKind::type, 110.0 },
        ArchiveColumnSpec{ ArchiveColumnKind::modified_time, 150.0 },
        ArchiveColumnSpec{ ArchiveColumnKind::original_size, 90.0 },
    };
    constexpr std::array archive_columns_with_compressed_size{
        ArchiveColumnSpec{ ArchiveColumnKind::name, 0.0 },
        ArchiveColumnSpec{ ArchiveColumnKind::type, 90.0 },
        ArchiveColumnSpec{ ArchiveColumnKind::compressed_size, 110.0 },
        ArchiveColumnSpec{ ArchiveColumnKind::original_size, 110.0 },
    };
    constexpr std::array archive_columns_without_compressed_size{
        ArchiveColumnSpec{ ArchiveColumnKind::name, 0.0 },
        ArchiveColumnSpec{ ArchiveColumnKind::type, 100.0 },
        ArchiveColumnSpec{ ArchiveColumnKind::original_size, 110.0 },
    };

    std::span<const ArchiveColumnSpec> archive_columns(
        bool is_directory,
        bool compressed_size_available) noexcept
    {
        if (is_directory)
        {
            return folder_columns;
        }
        return compressed_size_available
            ? std::span<const ArchiveColumnSpec>(archive_columns_with_compressed_size)
            : std::span<const ArchiveColumnSpec>(archive_columns_without_compressed_size);
    }

    void configure_archive_columns(
        const Grid& grid,
        std::span<const ArchiveColumnSpec> columns_to_add)
    {
        auto columns = grid.ColumnDefinitions();
        columns.Clear();
        for (const auto& spec : columns_to_add)
        {
            ColumnDefinition column;
            column.Width(spec.width == 0.0
                ? GridLength{ 1, GridUnitType::Star }
                : GridLength{ spec.width, GridUnitType::Pixel });
            columns.Append(column);
        }
    }

    int compare_case_insensitive(const std::wstring& left, const std::wstring& right) noexcept
    {
        return _wcsicmp(left.c_str(), right.c_str());
    }

    int compare_unsigned(std::uint64_t left, std::uint64_t right) noexcept
    {
        return left < right ? -1 : left > right ? 1 : 0;
    }

    void sort_folder_entries(
        std::vector<glance::app::ArchiveEntry>& entries,
        const glance::app::FolderPreviewPreferences& preferences)
    {
        std::ranges::sort(entries, [&preferences](const auto& left, const auto& right) {
            if (left.is_folder != right.is_folder)
            {
                return left.is_folder > right.is_folder;
            }

            int comparison{};
            switch (preferences.sort_field)
            {
            case glance::app::FolderSortField::type:
                comparison = compare_case_insensitive(left.type_name, right.type_name);
                break;
            case glance::app::FolderSortField::modified_time:
                comparison = compare_unsigned(left.modified_time, right.modified_time);
                break;
            case glance::app::FolderSortField::size:
                comparison = compare_unsigned(left.original_size, right.original_size);
                break;
            case glance::app::FolderSortField::name:
            default:
                comparison = compare_case_insensitive(left.name, right.name);
                break;
            }
            if (comparison == 0)
            {
                comparison = compare_case_insensitive(left.name, right.name);
            }
            return preferences.ascending ? comparison < 0 : comparison > 0;
        });
    }

    std::filesystem::path executable_directory()
    {
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

    Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap create_pdf_bitmap(
        const glance::app::PdfRenderResult& rendered)
    {
        Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap bitmap(
            static_cast<std::int32_t>(rendered.pixel_width),
            static_cast<std::int32_t>(rendered.pixel_height));
        auto access = bitmap.PixelBuffer().as<BufferByteAccess>();
        byte* destination{};
        winrt::check_hresult(access->Buffer(&destination));
        const std::size_t destination_stride =
            static_cast<std::size_t>(rendered.pixel_width) * 4U;
        for (std::uint32_t row = 0; row < rendered.pixel_height; ++row)
        {
            std::memcpy(
                destination + static_cast<std::size_t>(row) * destination_stride,
                rendered.pixels.data() + static_cast<std::size_t>(row) * rendered.stride,
                destination_stride);
        }
        bitmap.Invalidate();
        return bitmap;
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
    void MainWindow::OfficeConversionOperation::attach_process(HANDLE value) noexcept
    {
        std::scoped_lock lock(process_mutex);
        process = value;
        if (cancelled.load(std::memory_order_acquire) && process != nullptr)
        {
            TerminateProcess(process, ERROR_CANCELLED);
        }
    }

    void MainWindow::OfficeConversionOperation::detach_process(HANDLE value) noexcept
    {
        std::scoped_lock lock(process_mutex);
        if (process == value)
        {
            process = nullptr;
        }
    }

    void MainWindow::OfficeConversionOperation::cancel() noexcept
    {
        cancelled.store(true, std::memory_order_release);
        std::scoped_lock lock(process_mutex);
        if (process != nullptr)
        {
            TerminateProcess(process, ERROR_CANCELLED);
        }
    }

    bool MainWindow::OfficeConversionOperation::is_cancelled() const noexcept
    {
        return cancelled.load(std::memory_order_acquire);
    }

    MainWindow::MainWindow()
    {
        glance::contracts::log_event(L"MainWindow InitializeComponent begin.");
        InitializeComponent();
        glance::contracts::log_event(L"MainWindow InitializeComponent complete.");
        folder_preview_preferences_ = glance::app::load_folder_preview_preferences();
        ApplyLocalizedResources();
        ApplyAppearancePreferences();
        text_preferences_ = glance::app::load_text_preferences();
        footer_preferences_ = glance::app::load_footer_preferences();
        configure_window();
        ApplyWindowPreferences();
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

    void MainWindow::ApplyWindowPreferences()
    {
        if (window_ == nullptr)
        {
            return;
        }

        const auto preferences = glance::app::load_window_preferences();
        LONG_PTR extended_style = GetWindowLongPtrW(window_, GWL_EXSTYLE);
        if (preferences.opacity_percent < 100)
        {
            extended_style |= WS_EX_LAYERED;
            SetWindowLongPtrW(window_, GWL_EXSTYLE, extended_style);
            const BYTE alpha = static_cast<BYTE>(MulDiv(
                static_cast<int>(preferences.opacity_percent),
                255,
                100));
            SetLayeredWindowAttributes(window_, 0, alpha, LWA_ALPHA);
        }
        else if ((extended_style & WS_EX_LAYERED) != 0)
        {
            SetWindowLongPtrW(window_, GWL_EXSTYLE, extended_style & ~WS_EX_LAYERED);
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
        update_preview_mode_button();
        set_tooltip(
            GenericAdvancedInfoButton(),
            L"GenericAdvancedInfoButton.ToolTipService.ToolTip");
        LoadCloudFileText().Text(glance::app::localize(L"LoadCloudFileText.Text"));
        PreviewAsTextText().Text(glance::app::localize(L"PreviewAsTextText.Text"));
        if (preview_notice_active_)
        {
            PreviewErrorInfoBar().Message(
                glance::app::localize(L"SyntaxHighlightDisabledLargeFile"));
        }
        else
        {
            PreviewErrorInfoBar().Title(glance::app::localize(L"PreviewErrorInfoBar.Title"));
        }
        set_tooltip(MediaPlayPauseButton(), L"MediaPlayPauseButton.ToolTipService.ToolTip");
        set_tooltip(MediaMuteButton(), L"MediaMuteButton.ToolTipService.ToolTip");
        set_tooltip(PreviousPdfButton(), L"PreviousPdfButton.ToolTipService.ToolTip");
        set_tooltip(NextPdfButton(), L"NextPdfButton.ToolTipService.ToolTip");
        set_tooltip(PdfThumbnailsButton(), L"PdfThumbnailsButton.ToolTipService.ToolTip");
        set_tooltip(PdfOutlineButton(), L"PdfOutlineButton.ToolTipService.ToolTip");
        PasswordPromptTitle().Text(glance::app::localize(L"PasswordPromptTitle"));
        PasswordPromptInput().PlaceholderText(
            glance::app::localize(L"PasswordPromptInput.PlaceholderText"));
        PasswordPromptSubmitButton().Content(
            box_value(glance::app::localize(L"PasswordPromptSubmitButton.Content")));
        update_archive_header_state();
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
        update_line_number_visibility();
        update_generic_file_metadata();
        update_footer_metadata();
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
        update_preview_as_text_button();
    }

    void MainWindow::update_archive_header_state()
    {
        const auto columns = archive_columns(
            archive_preview_is_directory_,
            archive_entry_compressed_size_available_);
        configure_archive_columns(ArchiveHeaderGrid(), columns);
        ArchiveNameHeader().Text(glance::app::localize(L"ArchiveNameHeader.Text"));
        ArchiveTypeHeader().Text(glance::app::localize(L"ArchiveTypeHeader.Text"));
        ArchiveThirdHeader().Text(glance::app::localize(
            archive_preview_is_directory_
                ? L"ArchiveModifiedHeader.Text"
                : archive_entry_compressed_size_available_
                    ? L"ArchiveCompressedSizeHeader"
                    : L"ArchiveOriginalSizeHeader"));
        ArchiveFourthHeader().Text(glance::app::localize(
            archive_preview_is_directory_
                ? L"ArchiveSizeHeader.Text"
                : L"ArchiveOriginalSizeHeader"));
        ArchiveModifiedHeaderButton().HorizontalContentAlignment(
            archive_preview_is_directory_
                ? HorizontalAlignment::Left
                : HorizontalAlignment::Right);
        ArchiveSizeHeaderButton().Visibility(
            columns.size() > 3 ? Visibility::Visible : Visibility::Collapsed);

        const std::array buttons{
            ArchiveNameHeaderButton(),
            ArchiveTypeHeaderButton(),
            ArchiveModifiedHeaderButton(),
            ArchiveSizeHeaderButton(),
        };
        const std::array glyphs{
            ArchiveNameSortGlyph(),
            ArchiveTypeSortGlyph(),
            ArchiveModifiedSortGlyph(),
            ArchiveSizeSortGlyph(),
        };
        for (const auto& button : buttons)
        {
            const bool interactive =
                archive_preview_is_directory_ && button.Visibility() == Visibility::Visible;
            button.IsHitTestVisible(interactive);
            button.IsTabStop(interactive);
        }
        for (const auto& glyph : glyphs)
        {
            glyph.Visibility(Visibility::Collapsed);
        }
        FolderEntryList().Visibility(
            archive_preview_is_directory_ ? Visibility::Visible : Visibility::Collapsed);
        ArchiveEntryTree().Visibility(
            archive_preview_is_directory_ ? Visibility::Collapsed : Visibility::Visible);
        if (!archive_preview_is_directory_)
        {
            return;
        }

        const auto field_index = static_cast<std::size_t>(folder_preview_preferences_.sort_field);
        if (field_index < glyphs.size())
        {
            glyphs[field_index].Glyph(folder_preview_preferences_.ascending ? L"\xE70E" : L"\xE70D");
            glyphs[field_index].Visibility(Visibility::Visible);
        }
    }

    void MainWindow::ApplyFooterPreferences()
    {
        footer_preferences_ = glance::app::load_footer_preferences();
        update_footer_metadata();
        request_footer_access_if_needed();
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
            if (self == nullptr || !self->password_prompt_activation_enabled_)
            {
                return MA_NOACTIVATE;
            }
        }
        if (message == WM_GETMINMAXINFO)
        {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            const UINT dpi = GetDpiForWindow(window);
            limits->ptMinTrackSize.x = MulDiv(480, static_cast<int>(dpi), 96);
            limits->ptMinTrackSize.y = MulDiv(320, static_cast<int>(dpi), 96);
            return 0;
        }
        if (message == WM_ENTERSIZEMOVE && self != nullptr)
        {
            self->tracking_move_size_ = GetWindowRect(window, &self->move_size_start_bounds_) != FALSE;
        }
        if (message == WM_EXITSIZEMOVE && self != nullptr)
        {
            RECT bounds{};
            if (self->tracking_move_size_ && GetWindowRect(window, &bounds))
            {
                self->user_sized_ = self->user_sized_ ||
                    bounds.right - bounds.left !=
                        self->move_size_start_bounds_.right - self->move_size_start_bounds_.left ||
                    bounds.bottom - bounds.top !=
                        self->move_size_start_bounds_.bottom - self->move_size_start_bounds_.top;
            }
            self->tracking_move_size_ = false;
            self->save_current_window_placement();
        }
        if (message == WM_SYSCOMMAND && self != nullptr &&
            (wparam & 0xFFF0U) == SC_MAXIMIZE)
        {
            self->user_sized_ = true;
        }
        if (message == WM_NCDESTROY && self != nullptr)
        {
            glance::contracts::log_event(L"MainWindow received WM_NCDESTROY.");
            self->cancel_office_conversion();
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
        cancel_office_conversion();
        cancel_pdf_render();
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
        PdfThumbnailList().Items().Clear();
        PdfOutlineTree().RootNodes().Clear();
        hide_password_prompt();
        MarkdownPreviewWebView().Opacity(0.0);
        MarkdownPreviewWebView().Visibility(Visibility::Collapsed);
        TextContentRichText().Blocks().Clear();
        LongTextList().Items().Clear();
        LongTextList().Visibility(Visibility::Collapsed);
        set_line_number_text(L"");
        TextEncodingText().Text(L"");
        dismiss_preview_info_bar();
        if (font_size_overlay_timer_ != nullptr)
        {
            font_size_overlay_timer_.Stop();
        }
        TextFontSizeOverlay().Visibility(Visibility::Collapsed);
        archive_render_state_.reset();
        archive_preview_is_directory_ = false;
        archive_entry_compressed_size_available_ = false;
        archive_source_path_.clear();
        archive_password_.clear();
        update_archive_header_state();
        ArchiveEntryTree().RootNodes().Clear();
        FolderEntryList().Items().Clear();
        ArchiveStatusText().Text(L"");
        FileList().Items().Clear();
        FileList().Visibility(Visibility::Collapsed);
        FileListColumn().Width(GridLength{ 0, GridUnitType::Pixel });

        TitleText().Text(L"");
        FooterMetadataText().Text(L"");
        FileNameText().Text(L"");
        FilePathText().Text(L"");
        FileMetadataText().Text(L"");
        GenericFileIconImage().Source(nullptr);
        GenericFileIconImage().Visibility(Visibility::Collapsed);
        GenericFileFallbackIcon().Visibility(Visibility::Visible);
        GenericAdvancedInfoText().Text(L"");
        GenericAdvancedInfoScroller().Visibility(Visibility::Collapsed);
        LoadCloudFileButton().Visibility(Visibility::Collapsed);
        PreviewAsTextButton().Visibility(Visibility::Collapsed);
        GenericAdvancedInfoButton().Visibility(Visibility::Collapsed);
        PreviewModeButton().Visibility(Visibility::Collapsed);
        PreviewModeButton().IsChecked(false);
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
        current_text_reader_.reset();
        syntax_highlight_state_ = {};
        text_syntax_ranges_.clear();
        text_paragraph_ranges_.clear();
        text_render_tail_.clear();
        text_tail_block_attached_ = false;
        text_highlight_offset_ = 0;
        text_line_count_ = 0;
        current_text_has_more_ = false;
        text_chunk_loading_ = false;
        line_numbers_simple_ = false;
        virtual_line_numbers_ = false;
        long_text_mode_ = false;
        long_text_render_tail_.clear();
        long_text_block_ranges_.clear();
        long_text_block_start_lines_.clear();
        long_text_next_line_ = 1;
        image_metadata_.clear();
        media_dimensions_.clear();
        media_technical_info_.clear();
        footer_access_mode_.clear();
        footer_access_loaded_ = false;
        footer_access_requested_ = false;
        files_.clear();
        current_index_ = 0;
        source_kind_ = 0;
        source_window_ = nullptr;
        foreground_when_unpinned_ = nullptr;
        current_kind_ = glance::app::PreviewKind::generic;
        content_preview_kind_ = glance::app::PreviewKind::generic;
        basic_info_mode_ = false;
        generic_text_preview_allowed_ = false;
        media_is_audio_ = false;
        image_metadata_visible_ = false;
        image_panning_ = false;
        image_pixel_width_ = 0;
        image_pixel_height_ = 0;
        pdf_page_index_ = 0;
        pdf_page_count_ = 0;
        pdf_thumbnail_items_built_ = 0;
        pdf_source_path_.clear();
        pdf_password_.clear();
        pdf_outline_.clear();
        pdf_thumbnail_images_.clear();
        office_thumbnail_background_active_ = false;
        office_thumbnail_requested_.clear();
        pdf_wheel_delta_ = 0;

    }

    void MainWindow::cancel_office_conversion() noexcept
    {
        office_emf_preview_ = false;
        if (office_preview_client_ != nullptr)
        {
            office_preview_client_->cancel();
            office_preview_client_.reset();
        }
        if (office_conversion_ != nullptr)
        {
            office_conversion_->cancel();
            office_conversion_.reset();
        }
    }

    void MainWindow::cancel_pdf_render() noexcept
    {
        if (pdf_render_client_ != nullptr)
        {
            pdf_render_client_->cancel();
            pdf_render_client_.reset();
        }
    }

    void MainWindow::reset_hidden_window_size() noexcept
    {
        if (window_ == nullptr)
        {
            return;
        }
        const UINT dpi = GetDpiForWindow(window_);
        const auto preferences = glance::app::load_window_preferences();
        SetWindowPos(
            window_,
            nullptr,
            0,
            0,
            MulDiv(static_cast<int>(preferences.default_width), static_cast<int>(dpi), 96),
            MulDiv(static_cast<int>(preferences.default_height), static_cast<int>(dpi), 96),
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void MainWindow::position_initial_window(bool ignore_saved_size)
    {
        const auto preferences = glance::app::load_window_preferences();
        const auto storage_kind = basic_info_mode_ ? content_preview_kind_ : current_kind_;
        const auto saved_position = preferences.remember_position
            ? glance::app::load_window_position(storage_kind, media_is_audio_)
            : std::nullopt;
        HMONITOR monitor = saved_position
            ? MonitorFromPoint(*saved_position, MONITOR_DEFAULTTONEAREST)
            : MonitorFromWindow(
                  source_window_ != nullptr ? source_window_ : GetForegroundWindow(),
                  MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{ sizeof(MONITORINFO) };
        GetMonitorInfoW(monitor, &info);

        const UINT dpi = source_window_ != nullptr ? GetDpiForWindow(source_window_) : 96;
        int desired_width = MulDiv(static_cast<int>(preferences.default_width), static_cast<int>(dpi), 96);
        int desired_height = MulDiv(static_cast<int>(preferences.default_height), static_cast<int>(dpi), 96);
        if (preferences.remember_size && !ignore_saved_size && !auto_fit_applies())
        {
            if (const auto saved_size = glance::app::load_window_size(storage_kind, media_is_audio_))
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
        const int x = saved_position
            ? std::clamp(saved_position->x, info.rcWork.left, std::max(info.rcWork.left, info.rcWork.right - width))
            : info.rcWork.left + (work_width - width) / 2;
        const int y = saved_position
            ? std::clamp(saved_position->y, info.rcWork.top, std::max(info.rcWork.top, info.rcWork.bottom - height))
            : info.rcWork.top + (work_height - height) / 2;

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
        if (user_sized_ || !glance::app::load_window_preferences().auto_fit_media)
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
        int horizontal_chrome = std::max(
            0,
            current_width - static_cast<int>(std::lround(panel.ActualWidth())));
        if (current_kind_ == glance::app::PreviewKind::pdf ||
            current_kind_ == glance::app::PreviewKind::office)
        {
            horizontal_chrome += static_cast<int>(std::lround(PdfNavigationColumn().ActualWidth()));
        }
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
        const bool preserve_position = glance::app::load_window_preferences().remember_position;
        const int x = preserve_position
            ? std::clamp(bounds.left, info.rcWork.left, std::max(info.rcWork.left, info.rcWork.right - width))
            : info.rcWork.left + (work_width - width) / 2;
        const int y = preserve_position
            ? std::clamp(bounds.top, info.rcWork.top, std::max(info.rcWork.top, info.rcWork.bottom - height))
            : info.rcWork.top + (work_height - height) / 2;
        SetWindowPos(window_, nullptr, x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
    }

    void MainWindow::save_current_window_placement() const noexcept
    {
        if (!visible_ || window_ == nullptr || IsZoomed(window_) || detached_ ||
            (pinned_ && topmost_))
        {
            return;
        }
        const auto preferences = glance::app::load_window_preferences();
        if (!preferences.remember_size && !preferences.remember_position)
        {
            return;
        }
        RECT bounds{};
        if (!GetWindowRect(window_, &bounds))
        {
            return;
        }
        const auto storage_kind = basic_info_mode_ ? content_preview_kind_ : current_kind_;
        if (preferences.remember_size && user_sized_)
        {
            const UINT dpi = GetDpiForWindow(window_);
            glance::app::save_window_size(
                storage_kind,
                SIZE{
                    MulDiv(bounds.right - bounds.left, 96, static_cast<int>(dpi)),
                    MulDiv(bounds.bottom - bounds.top, 96, static_cast<int>(dpi)) },
                media_is_audio_);
        }
        if (preferences.remember_position)
        {
            glance::app::save_window_position(
                storage_kind,
                POINT{ bounds.left, bounds.top },
                media_is_audio_);
        }
    }

    void MainWindow::present_file(std::uint32_t index)
    {
        if (index >= files_.size())
        {
            return;
        }
        cancel_office_conversion();
        hide_password_prompt();
        current_index_ = index;
        basic_info_mode_ = false;
        content_preview_kind_ = glance::app::PreviewKind::generic;
        generic_text_preview_allowed_ = false;
        archive_render_state_.reset();
        archive_preview_is_directory_ = false;
        archive_entry_compressed_size_available_ = false;
        update_archive_header_state();
        update_preview_mode_button();
        dismiss_preview_info_bar();
        const auto& file = files_[index];
        const auto generation = ++content_generation_;
        TitleText().Text(file.display_name);
        image_pixel_width_ = 0;
        image_pixel_height_ = 0;
        media_dimensions_.clear();
        media_technical_info_.clear();
        footer_access_mode_.clear();
        footer_access_loaded_ = false;
        footer_access_requested_ = false;
        update_footer_metadata();
        request_footer_access_if_needed();

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
            folder_preview_preferences_ = glance::app::load_folder_preview_preferences();
            archive_preview_is_directory_ = true;
            update_archive_header_state();
            content_preview_kind_ = glance::app::PreviewKind::archive;
            update_preview_mode_button();
            current_kind_ = glance::app::PreviewKind::archive;
            show_content_panel(current_kind_);
            ArchiveStatusText().Text(glance::app::localize(L"LoadingFolder"));
            ArchiveEntryTree().RootNodes().Clear();
            FolderEntryList().Items().Clear();
            load_directory_async(file.path, generation);
            return;
        }

        const auto kind = glance::app::resolve_preview_kind(file.path);
        if (kind == glance::app::PreviewKind::office &&
            !glance::app::office_preview_available(file.path))
        {
            content_preview_kind_ = glance::app::PreviewKind::generic;
            update_preview_mode_button();
            current_kind_ = glance::app::PreviewKind::generic;
            present_generic(file, false, true);
            return;
        }
        content_preview_kind_ = kind;
        update_preview_mode_button();
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
            image_pixel_width_ = 0;
            image_pixel_height_ = 0;
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
            media_seek_wheel_delta_ = 0;
            media_volume_wheel_delta_ = 0;
            {
                const auto preferences = glance::app::load_media_preview_preferences();
                MediaVolumeSlider().Value(
                    media_is_audio_
                        ? preferences.audio_volume_percent
                        : preferences.video_volume_percent);
            }
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
            PdfPageImage().Source(nullptr);
            pdf_wheel_delta_ = 0;
            PdfPageText().Text(glance::app::localize(L"Loading"));
            PdfLoadingText().Text(glance::app::localize(L"LoadingPdf"));
            PdfLoadingText().Visibility(Visibility::Visible);
            PdfLoadingOverlay().Visibility(Visibility::Visible);
            load_pdf_async(file.path, generation);
            break;
        case glance::app::PreviewKind::archive:
            show_content_panel(kind);
            ArchiveStatusText().Text(glance::app::localize(L"LoadingArchive"));
            ArchiveEntryTree().RootNodes().Clear();
            FolderEntryList().Items().Clear();
            load_archive_async(file.path, generation);
            break;
        case glance::app::PreviewKind::office:
            show_content_panel(glance::app::PreviewKind::pdf);
            cancel_pdf_render();
            PdfPageImage().Source(nullptr);
            pdf_wheel_delta_ = 0;
            PdfPageText().Text(L"1 / 1");
            PdfLoadingText().Text(glance::app::localize(L"ConvertingOffice"));
            PdfLoadingText().Visibility(Visibility::Visible);
            PdfLoadingOverlay().Visibility(Visibility::Visible);
            load_office_async(file.path, generation, file.size, file.last_write_time);
            break;
        default:
            present_generic(file, true, true);
            break;
        }
    }

    void MainWindow::present_generic(
        const glance::app::PreviewFile& file,
        bool allow_text_preview,
        bool allow_advanced_info)
    {
        current_kind_ = glance::app::PreviewKind::generic;
        show_content_panel(glance::app::PreviewKind::generic);
        FileNameText().Text(file.display_name);
        FilePathText().Text(!file.path.empty() ? file.path : file.parsing_name);
        update_generic_file_metadata();
        GenericFileIconImage().Source(nullptr);
        GenericFileIconImage().Visibility(Visibility::Collapsed);
        GenericFileFallbackIcon().Visibility(Visibility::Visible);
        GenericAdvancedInfoText().Text(L"");
        GenericAdvancedInfoScroller().Visibility(Visibility::Collapsed);
        generic_preview_preferences_ = glance::app::load_generic_preview_preferences();
        GenericAdvancedInfoButton().IsChecked(generic_preview_preferences_.show_advanced_info);
        LoadCloudFileButton().Visibility(file.is_cloud_placeholder ? Visibility::Visible : Visibility::Collapsed);
        generic_text_preview_allowed_ = allow_text_preview;
        update_preview_as_text_button();
        PreviewAsTextButton().IsEnabled(true);
        const bool advanced_info_available =
            allow_advanced_info && !file.path.empty() && !file.is_cloud_placeholder &&
            (file.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
        GenericAdvancedInfoButton().Visibility(
            advanced_info_available ? Visibility::Visible : Visibility::Collapsed);
        ErrorText().Visibility(Visibility::Collapsed);
        const auto icon_path = !file.path.empty() ? file.path : file.parsing_name;
        if (!icon_path.empty())
        {
            load_generic_icon_async(
                icon_path,
                (file.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                file.is_cloud_placeholder || file.path.empty(),
                content_generation_);
        }
        if (advanced_info_available && generic_preview_preferences_.show_advanced_info)
        {
            load_generic_file_info_async(file.path, content_generation_);
        }
        update_footer_metadata();
    }

    fire_and_forget MainWindow::load_generic_icon_async(
        std::wstring path,
        bool is_folder,
        bool use_file_attributes,
        std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        co_await resume_background();
        auto bitmap = glance::app::load_shell_icon(path, is_folder, 64, use_file_attributes);
        if (bitmap == nullptr)
        {
            co_return;
        }
        static_cast<void>(dispatcher.TryEnqueue([lifetime, bitmap = std::move(bitmap), generation]() {
            if (generation != lifetime->content_generation_ ||
                lifetime->current_kind_ != glance::app::PreviewKind::generic)
            {
                return;
            }
            try
            {
                const auto source = glance::app::create_shell_icon_source(*bitmap);
                if (source == nullptr)
                {
                    return;
                }
                lifetime->GenericFileIconImage().Source(source);
                lifetime->GenericFileIconImage().Visibility(Visibility::Visible);
                lifetime->GenericFileFallbackIcon().Visibility(Visibility::Collapsed);
            }
            catch (...)
            {
            }
        }));
    }

    fire_and_forget MainWindow::load_generic_file_info_async(std::wstring path, std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        co_await resume_background();
        auto info = glance::app::load_generic_file_info(path);
        static_cast<void>(dispatcher.TryEnqueue([lifetime, generation, info = std::move(info)]() mutable {
            if (generation != lifetime->content_generation_ ||
                lifetime->current_kind_ != glance::app::PreviewKind::generic ||
                !lifetime->generic_preview_preferences_.show_advanced_info ||
                info.empty())
            {
                return;
            }
            lifetime->GenericAdvancedInfoText().Text(std::move(info));
            lifetime->GenericAdvancedInfoScroller().Visibility(Visibility::Visible);
        }));
    }

    fire_and_forget MainWindow::load_footer_access_async(std::wstring path, std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        co_await resume_background();
        auto access = glance::app::load_file_access_mode(path);
        static_cast<void>(dispatcher.TryEnqueue(
            [lifetime, generation, access = std::move(access)]() mutable {
                if (generation != lifetime->content_generation_)
                {
                    return;
                }
                lifetime->footer_access_requested_ = false;
                lifetime->footer_access_loaded_ = true;
                lifetime->footer_access_mode_ = access.value_or(L"--");
                lifetime->update_footer_metadata();
            }));
    }

    void MainWindow::prepare_text_preview(const glance::app::PreviewFile& file, bool markdown)
    {
        current_kind_ = markdown ? glance::app::PreviewKind::markdown : glance::app::PreviewKind::text;
        show_content_panel(markdown ? glance::app::PreviewKind::markdown : glance::app::PreviewKind::text);
        current_text_.clear();
        current_text_path_ = file.path;
        current_text_markdown_ = markdown;
        current_text_reader_.reset();
        syntax_highlight_state_ = {};
        text_syntax_ranges_.clear();
        text_paragraph_ranges_.clear();
        text_render_tail_.clear();
        text_tail_block_attached_ = false;
        text_highlight_offset_ = 0;
        text_line_count_ = 0;
        current_text_has_more_ = false;
        text_chunk_loading_ = true;
        line_numbers_simple_ = false;
        virtual_line_numbers_ = false;
        current_text_encoding_ = glance::app::TextEncoding::automatic;
        markdown_preview_ = markdown;
        EncodingSelector().Content(box_value(glance::app::localize(L"EncodingDetecting")));
        apply_text_preferences();
        syntax_highlight_notice_pending_ =
            file.size > automatic_syntax_highlight_limit_bytes && syntax_highlighting_;
        if (syntax_highlight_notice_pending_)
        {
            syntax_highlighting_ = false;
            SyntaxHighlightButton().IsChecked(false);
        }
        current_text_ = glance::app::localize(L"Loading");
        render_text_content();
        set_line_number_text(L"");
        MarkdownModeButtons().Visibility(markdown ? Visibility::Visible : Visibility::Collapsed);
        set_markdown_preview_mode(markdown);
        if (markdown)
        {
            MarkdownPreviewWebView().Opacity(0.0);
        }
    }

    void MainWindow::present_text(const glance::app::PreviewFile& file, bool markdown)
    {
        prepare_text_preview(file, markdown);
        load_text_async(file.path, markdown, content_generation_, current_text_encoding_);
    }

    fire_and_forget MainWindow::load_text_async(
        std::wstring path,
        bool markdown,
        std::uint64_t generation,
        glance::app::TextEncoding encoding,
        bool preview_as_text_attempt)
    {
        text_chunk_loading_ = true;
        current_text_has_more_ = false;
        current_text_reader_.reset();
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        co_await resume_background();
        auto preview = glance::app::load_text_preview(path, text_chunk_bytes, encoding);
        static_cast<void>(dispatcher.TryEnqueue(
            [lifetime, preview = std::move(preview), markdown, generation, preview_as_text_attempt]() mutable {
                lifetime->apply_text_preview(
                    std::move(preview),
                    markdown,
                    generation,
                    preview_as_text_attempt);
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
                lifetime->image_pixel_width_ = width;
                lifetime->image_pixel_height_ = height;
                lifetime->ImagePreview().Source(bitmap);
                lifetime->fit_image_to_viewport();
                lifetime->update_footer_metadata();
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
            MediaPreview().MediaPlayer().IsMuted(false);
            MediaPreview().MediaPlayer().Volume(MediaVolumeSlider().Value() / 100.0);
            const auto preferences = glance::app::load_media_preview_preferences();
            if (media_is_audio_ ? preferences.autoplay_audio : preferences.autoplay_video)
            {
                MediaPreview().MediaPlayer().Play();
            }
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
        update_footer_metadata();
    }

    void MainWindow::update_footer_metadata()
    {
        if (current_index_ >= files_.size())
        {
            FooterMetadataText().Text(L"");
            return;
        }

        const auto& file = files_[current_index_];
        std::vector<std::wstring> fields;
        const auto append = [&fields](std::wstring value) {
            if (!value.empty())
            {
                fields.push_back(std::move(value));
            }
        };
        for (const auto field : footer_preferences_.order)
        {
            if (!glance::app::footer_field_enabled(footer_preferences_, field))
            {
                continue;
            }
            switch (field)
            {
            case glance::app::FooterField::size:
                append(formatted_size(file.size));
                break;
            case glance::app::FooterField::modified_time:
                if (file.last_write_time != 0)
                {
                    append(glance::app::localize_format(
                        L"FooterModifiedTimeFormat",
                        { formatted_time(file.last_write_time) }));
                }
                break;
            case glance::app::FooterField::creation_time:
                if (file.creation_time != 0)
                {
                    append(glance::app::localize_format(
                        L"FooterCreationTimeFormat",
                        { formatted_time(file.creation_time) }));
                }
                break;
            case glance::app::FooterField::permissions:
                append(footer_access_loaded_ ? footer_access_mode_ : L"--");
                break;
            }
        }

        if (current_kind_ == glance::app::PreviewKind::image &&
            image_pixel_width_ > 0 && image_pixel_height_ > 0)
        {
            append(std::to_wstring(image_pixel_width_) + L" x " + std::to_wstring(image_pixel_height_));
        }
        if (current_kind_ == glance::app::PreviewKind::media)
        {
            append(media_dimensions_);
            append(media_technical_info_);
        }

        std::wstring metadata;
        for (const auto& field : fields)
        {
            metadata += metadata.empty() ? field : L"  |  " + field;
        }
        FooterMetadataText().Text(metadata);
    }

    void MainWindow::update_generic_file_metadata()
    {
        if (current_kind_ != glance::app::PreviewKind::generic || current_index_ >= files_.size())
        {
            return;
        }
        const auto& file = files_[current_index_];
        std::wstring metadata = formatted_size(file.size);
        if (file.last_write_time != 0)
        {
            metadata += L"  |  " + glance::app::localize_format(
                L"GenericModifiedAt",
                { formatted_time(file.last_write_time) });
        }
        if (file.creation_time != 0)
        {
            metadata += L"  |  " + glance::app::localize_format(
                L"GenericCreatedAt",
                { formatted_time(file.creation_time) });
        }
        FileMetadataText().Text(metadata);
    }

    void MainWindow::request_footer_access_if_needed()
    {
        if (!glance::app::footer_field_enabled(
                footer_preferences_, glance::app::FooterField::permissions) ||
            footer_access_loaded_ || footer_access_requested_ ||
            current_index_ >= files_.size())
        {
            return;
        }

        const auto& file = files_[current_index_];
        if (file.path.empty() || file.is_cloud_placeholder)
        {
            footer_access_mode_ = L"--";
            footer_access_loaded_ = true;
            update_footer_metadata();
            return;
        }
        footer_access_requested_ = true;
        load_footer_access_async(file.path, content_generation_);
    }

    fire_and_forget MainWindow::load_pdf_async(
        std::wstring path,
        std::uint64_t generation,
        std::wstring password)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        cancel_pdf_render();
        auto session = glance::app::acquire_pdf_render_client();
        pdf_render_client_ = session;
        pdf_source_path_ = path;
        pdf_password_ = password;
        PdfLoadingText().Visibility(Visibility::Visible);
        co_await resume_background();
        auto result = session->open(path, password);
        static_cast<void>(dispatcher.TryEnqueue([
            lifetime,
            session,
            result = std::move(result),
            path = std::move(path),
            password = std::move(password),
            generation]() mutable {
            lifetime->apply_pdf_open_result(
                std::move(session),
                std::move(result),
                std::move(path),
                std::move(password),
                generation);
        }));
    }

    void MainWindow::apply_pdf_open_result(
        std::shared_ptr<glance::app::PdfRenderClient> session,
        glance::app::PdfOpenResult result,
        std::wstring path,
        std::wstring password,
        std::uint64_t generation)
    {
        using glance::contracts::pdf::Status;
        glance::contracts::log_event(
            L"PDF open completed: status=" +
            std::to_wstring(static_cast<std::uint32_t>(result.status)) +
            L", pages=" + std::to_wstring(result.page_count));
        if (generation != content_generation_ || session != pdf_render_client_)
        {
            session->cancel();
            return;
        }
        if (result.status == Status::password_required || result.status == Status::invalid_password)
        {
            PdfLoadingOverlay().Visibility(Visibility::Collapsed);
            pdf_source_path_ = std::move(path);
            pdf_password_ = std::move(password);
            show_password_prompt(
                PasswordPromptTarget::pdf,
                result.status == Status::invalid_password);
            return;
        }
        if (result.status != Status::success)
        {
            show_provider_error(
                result.status == Status::dependency_missing
                    ? glance::app::localize(L"PdfComponentMissingError")
                    : glance::app::localize(L"PdfOpenError"),
                generation);
            return;
        }
        if (result.page_count == 0)
        {
            show_provider_error(glance::app::localize(L"PdfEmptyError"), generation);
            return;
        }
        hide_password_prompt();
        pdf_page_count_ = result.page_count;
        pdf_outline_ = std::move(result.outline);
        pdf_page_index_ = 0;
        render_pdf_page_async(pdf_page_index_, generation);
        build_pdf_navigation(generation);
    }

    fire_and_forget MainWindow::render_pdf_page_async(
        std::uint32_t page_index,
        std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        const auto session = pdf_render_client_;
        const auto request = pdf_render_request_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (session == nullptr || page_index >= pdf_page_count_)
        {
            co_return;
        }
        if (PdfPageImage().Source() != nullptr)
        {
            PdfLoadingText().Visibility(Visibility::Collapsed);
        }
        PdfLoadingOverlay().Visibility(Visibility::Visible);
        const auto width = static_cast<std::uint32_t>(std::clamp(
            std::lround(std::max(512.0, PdfScroller().ActualWidth()) * 2.0),
            1024L,
            4096L));
        const auto height = static_cast<std::uint32_t>(std::clamp(
            std::lround(std::max(512.0, PdfScroller().ActualHeight()) * 2.0),
            1024L,
            4096L));
        co_await resume_background();
        if (request != pdf_render_request_.load(std::memory_order_relaxed))
        {
            co_return;
        }
        auto rendered = session->render(page_index, width, height);
        static_cast<void>(dispatcher.TryEnqueue([
            lifetime,
            session,
            rendered = std::move(rendered),
            page_index,
            request,
            generation]() mutable {
            using glance::contracts::pdf::Status;
            if (generation != lifetime->content_generation_ ||
                session != lifetime->pdf_render_client_ ||
                page_index != lifetime->pdf_page_index_ ||
                request != lifetime->pdf_render_request_.load(std::memory_order_relaxed))
            {
                return;
            }
            if (rendered.status != Status::success)
            {
                lifetime->show_provider_error(
                    glance::app::localize(L"PdfRenderError"),
                    generation);
                return;
            }
            try
            {
                lifetime->PdfPageImage().Source(create_pdf_bitmap(rendered));
                lifetime->PdfLoadingOverlay().Visibility(Visibility::Collapsed);
                lifetime->PdfPageText().Text(
                    std::to_wstring(page_index + 1) + L" / " +
                    std::to_wstring(lifetime->pdf_page_count_));
                lifetime->sync_pdf_thumbnail_selection();
                lifetime->auto_fit_window_to_content(
                    rendered.page_width_points,
                    rendered.page_height_points);
            }
            catch (const hresult_error& error)
            {
                lifetime->show_provider_error(
                    glance::app::localize_format(L"PdfRenderErrorDetail", { error.message() }),
                    generation);
            }
        }));
    }

    fire_and_forget MainWindow::load_pdf_thumbnails_async(std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        const auto session = pdf_render_client_;
        const auto page_count = pdf_page_count_;
        co_await resume_background();
        for (std::uint32_t page = 0; page < page_count; ++page)
        {
            auto rendered = session->render(page, 176, 132);
            if (rendered.status != glance::contracts::pdf::Status::success)
            {
                co_return;
            }
            if (!dispatcher.TryEnqueue([
                    lifetime,
                    session,
                    rendered = std::move(rendered),
                    page,
                    generation]() mutable {
                    if (generation != lifetime->content_generation_ ||
                        session != lifetime->pdf_render_client_ ||
                        page >= lifetime->pdf_thumbnail_images_.size())
                    {
                        return;
                    }
                    try
                    {
                        if (auto image = lifetime->pdf_thumbnail_images_[page].get())
                        {
                            image.Source(create_pdf_bitmap(rendered));
                        }
                    }
                    catch (...)
                    {
                    }
                }))
            {
                co_return;
            }
            co_await resume_after(std::chrono::milliseconds(8));
        }
    }

    fire_and_forget MainWindow::load_archive_async(
        std::wstring path,
        std::uint64_t generation,
        std::wstring password)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        archive_source_path_ = path;
        archive_password_ = password;
        co_await resume_background();
        auto preview = glance::app::load_archive_preview(path, password);
        static_cast<void>(dispatcher.TryEnqueue([
            lifetime,
            preview = std::move(preview),
            path = std::move(path),
            password = std::move(password),
            generation]() mutable {
                if (generation != lifetime->content_generation_)
                {
                    return;
                }
                lifetime->archive_source_path_ = std::move(path);
                lifetime->archive_password_ = std::move(password);
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
        if (preview.password_required || preview.invalid_password)
        {
            show_password_prompt(
                PasswordPromptTarget::archive,
                preview.invalid_password);
            return;
        }
        if (!preview.error.empty())
        {
            show_provider_error(std::move(preview.error), generation);
            return;
        }

        archive_entry_compressed_size_available_ =
            !archive_preview_is_directory_ && preview.entry_compressed_size_available;
        update_archive_header_state();
        auto root_nodes = ArchiveEntryTree().RootNodes();
        root_nodes.Clear();
        FolderEntryList().Items().Clear();
        if (archive_preview_is_directory_)
        {
            sort_folder_entries(preview.entries, folder_preview_preferences_);
        }
        auto state = std::make_shared<ArchiveRenderState>();
        state->preview = std::move(preview);
        state->generation = generation;
        state->icon_targets.reserve(state->preview.entry_count);
        for (const auto& entry : state->preview.entries)
        {
            state->pending.push_back(PendingArchiveNode{ &entry, nullptr });
        }
        state->status = glance::app::localize_format(
            L"FileCount",
            { std::to_wstring(state->preview.file_count) });
        if (archive_preview_is_directory_)
        {
            state->status += L"  |  " + glance::app::localize_format(
                state->preview.truncated ? L"PartialTotalSize" : L"TotalSize",
                { state->preview.original_size_known
                    ? formatted_size(state->preview.original_size)
                    : std::wstring(L"--") });
        }
        else
        {
            const auto compressed_size = state->preview.compressed_size_known
                ? formatted_size(state->preview.compressed_size)
                : std::wstring(L"--");
            const auto original_size = state->preview.original_size_known
                ? formatted_size(state->preview.original_size)
                : std::wstring(L"--");
            std::wstring compression_ratio{ L"--" };
            if (state->preview.compressed_size_known &&
                state->preview.original_size_known &&
                state->preview.original_size != 0)
            {
                std::wostringstream value;
                value << std::fixed << std::setprecision(1)
                      << static_cast<double>(state->preview.compressed_size) * 100.0 /
                             static_cast<double>(state->preview.original_size)
                      << L'%';
                compression_ratio = value.str();
            }
            state->status += L"  |  " + glance::app::localize_format(
                L"ArchiveCompressedSize",
                { compressed_size });
            state->status += L"  |  " + glance::app::localize_format(
                L"ArchiveOriginalSize",
                { original_size });
            state->status += L"  |  " + glance::app::localize_format(
                L"ArchiveCompressionRatio",
                { compression_ratio });
        }
        if (state->preview.truncated)
        {
            state->status += L"  |  " + glance::app::localize(L"ListTruncated");
        }
        if (state->preview.depth_limited)
        {
            state->status += L"  |  " + glance::app::localize(L"ArchiveDepthLimited");
        }
        archive_render_state_ = state;
        render_archive_batch(state);
    }

    void MainWindow::render_archive_batch(const std::shared_ptr<ArchiveRenderState>& state)
    {
        if (state->generation != content_generation_ ||
            archive_render_state_ != state ||
            current_kind_ != glance::app::PreviewKind::archive)
        {
            return;
        }

        constexpr std::size_t batch_size = 64;
        auto root_nodes = ArchiveEntryTree().RootNodes();
        auto folder_items = FolderEntryList().Items();
        for (std::size_t index = 0; index < batch_size && !state->pending.empty(); ++index)
        {
            const auto pending = std::move(state->pending.front());
            state->pending.pop_front();
            const auto& entry = *pending.entry;

            Grid row;
            const auto columns = archive_columns(
                archive_preview_is_directory_,
                archive_entry_compressed_size_available_);
            configure_archive_columns(row, columns);

            Grid name_cell;
            name_cell.Margin(Thickness{ 8, 0, 8, 0 });
            ColumnDefinition icon_column;
            icon_column.Width(GridLength{ 28, GridUnitType::Pixel });
            name_cell.ColumnDefinitions().Append(icon_column);
            ColumnDefinition name_text_column;
            name_text_column.Width(GridLength{ 1, GridUnitType::Star });
            name_cell.ColumnDefinitions().Append(name_text_column);

            Grid icon_host;
            icon_host.Width(20);
            icon_host.Height(20);
            icon_host.HorizontalAlignment(HorizontalAlignment::Left);
            icon_host.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(icon_host, 0);

            Image icon_image;
            icon_image.Width(16);
            icon_image.Height(16);
            icon_image.Stretch(Media::Stretch::Uniform);
            icon_image.Visibility(Visibility::Collapsed);
            icon_host.Children().Append(icon_image);

            FontIcon fallback_icon;
            fallback_icon.FontSize(13);
            fallback_icon.Glyph(entry.is_folder ? L"\xE8B7" : L"\xE8A5");
            icon_host.Children().Append(fallback_icon);
            name_cell.Children().Append(icon_host);

            TextBlock name_text;
            name_text.Text(entry.name);
            name_text.FontSize(12);
            name_text.VerticalAlignment(VerticalAlignment::Center);
            name_text.TextTrimming(TextTrimming::CharacterEllipsis);
            Grid::SetColumn(name_text, 1);
            name_cell.Children().Append(name_text);
            Grid::SetColumn(name_cell, 0);
            row.Children().Append(name_cell);

            const auto icon_path = entry.path.empty() ? entry.name : entry.path;
            auto extension = entry.is_folder
                ? std::wstring(L":folder")
                : std::filesystem::path(icon_path).extension().wstring();
            std::ranges::transform(extension, extension.begin(), [](wchar_t value) {
                return static_cast<wchar_t>(std::towlower(value));
            });
            if (extension.empty())
            {
                extension = L":file";
            }
            state->icon_targets.push_back(ArchiveIconTarget{
                icon_path,
                std::move(extension),
                entry.is_folder,
                make_weak(icon_image),
                make_weak(fallback_icon) });

            const auto append_text = [&row](std::wstring_view value, int column, TextAlignment alignment = TextAlignment::Left) {
                TextBlock text;
                text.Text(value);
                text.FontSize(12);
                text.VerticalAlignment(VerticalAlignment::Center);
                text.TextAlignment(alignment);
                text.TextTrimming(TextTrimming::CharacterEllipsis);
                text.Margin(Thickness{ 8, 0, 8, 0 });
                Grid::SetColumn(text, column);
                row.Children().Append(text);
            };
            for (std::size_t column = 1; column < columns.size(); ++column)
            {
                switch (columns[column].kind)
                {
                case ArchiveColumnKind::type:
                    append_text(entry.type_name, static_cast<int>(column));
                    break;
                case ArchiveColumnKind::modified_time:
                    append_text(
                        entry.modified_time == 0 ? L"" : formatted_time(entry.modified_time),
                        static_cast<int>(column));
                    break;
                case ArchiveColumnKind::compressed_size:
                    append_text(
                        entry.is_folder
                            ? L""
                            : entry.compressed_size_known
                                ? formatted_size(entry.compressed_size)
                                : L"--",
                        static_cast<int>(column),
                        TextAlignment::Right);
                    break;
                case ArchiveColumnKind::original_size:
                    append_text(
                        entry.is_folder
                            ? L""
                            : entry.original_size_known
                                ? formatted_size(entry.original_size)
                                : L"--",
                        static_cast<int>(column),
                        TextAlignment::Right);
                    break;
                case ArchiveColumnKind::name:
                default:
                    break;
                }
            }

            if (archive_preview_is_directory_)
            {
                ListViewItem item;
                item.Content(row);
                folder_items.Append(item);
            }
            else
            {
                TreeViewNode node;
                node.Content(row);
                if (pending.parent != nullptr)
                {
                    pending.parent.Children().Append(node);
                }
                else
                {
                    root_nodes.Append(node);
                }
                for (const auto& child : entry.children)
                {
                    state->pending.push_back(PendingArchiveNode{ &child, node });
                }
            }
        }

        if (!state->pending.empty())
        {
            const auto lifetime = get_strong();
            static_cast<void>(DispatcherQueue().TryEnqueue([lifetime, state] {
                lifetime->render_archive_batch(state);
            }));
            return;
        }

        ArchiveStatusText().Text(state->status);
        if (!state->icon_targets.empty())
        {
            load_archive_icons_async(std::move(state->icon_targets), state->generation);
        }
    }

    fire_and_forget MainWindow::load_archive_icons_async(
        std::vector<ArchiveIconTarget> targets,
        std::uint64_t generation)
    {
        std::unordered_map<std::wstring, std::vector<ArchiveIconTarget>> groups;
        for (auto& target : targets)
        {
            groups[target.cache_key].push_back(std::move(target));
        }

        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        co_await resume_background();
        for (auto& [cache_key, group] : groups)
        {
            static_cast<void>(cache_key);
            const auto& representative = group.front();
            auto bitmap = glance::app::load_shell_icon(
                representative.path,
                representative.is_folder,
                16,
                true);
            if (bitmap == nullptr)
            {
                continue;
            }

            static_cast<void>(dispatcher.TryEnqueue(
                [lifetime, bitmap = std::move(bitmap), group = std::move(group), generation]() mutable {
                    if (generation != lifetime->content_generation_ ||
                        lifetime->current_kind_ != glance::app::PreviewKind::archive)
                    {
                        return;
                    }
                    try
                    {
                        const auto source = glance::app::create_shell_icon_source(*bitmap);
                        if (source == nullptr)
                        {
                            return;
                        }
                        for (auto& target : group)
                        {
                            const auto image = target.image.get();
                            const auto fallback = target.fallback.get();
                            if (image == nullptr || fallback == nullptr)
                            {
                                continue;
                            }
                            image.Source(source);
                            image.Visibility(Visibility::Visible);
                            fallback.Visibility(Visibility::Collapsed);
                        }
                    }
                    catch (...)
                    {
                    }
                }));
        }
    }

    fire_and_forget MainWindow::load_word_emf_async(
        std::wstring path,
        std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        auto office_session = std::make_shared<glance::app::OfficePreviewClient>();
        auto render_session = glance::app::acquire_pdf_render_client();
        office_preview_client_ = office_session;
        pdf_render_client_ = render_session;
        office_emf_preview_ = true;
        office_thumbnail_background_active_ = false;
        pdf_page_count_ = 1;
        pdf_page_index_ = 0;
        pdf_outline_.clear();
        office_thumbnail_requested_.clear();
        const auto width = static_cast<std::uint32_t>(std::clamp(
            std::lround(std::max(512.0, PdfScroller().ActualWidth()) * 2.0),
            1024L,
            4096L));
        const auto height = static_cast<std::uint32_t>(std::clamp(
            std::lround(std::max(512.0, PdfScroller().ActualHeight()) * 2.0),
            1024L,
            4096L));

        co_await resume_background();
        glance::app::OfficePageResult page;
        glance::app::PdfRenderResult rendered;
        bool opened{};
        try
        {
            opened = office_session->open_word(path);
            if (opened)
            {
                page = office_session->render_page(0);
                if (page.status == glance::contracts::office::Status::success)
                {
                    rendered = render_session->render_emf(page.emf_path, 0, width, height);
                }
            }
        }
        catch (...)
        {
            opened = false;
        }
        if (!opened || page.status != glance::contracts::office::Status::success ||
            rendered.status != glance::contracts::pdf::Status::success)
        {
            office_session->cancel();
            render_session->cancel();
            static_cast<void>(dispatcher.TryEnqueue([lifetime, office_session, render_session, generation] {
                if (generation != lifetime->content_generation_ ||
                    office_session != lifetime->office_preview_client_ ||
                    render_session != lifetime->pdf_render_client_)
                {
                    return;
                }
                lifetime->show_provider_error(
                    glance::app::localize(L"OfficeConvertError"),
                    generation);
            }));
            co_return;
        }

        static_cast<void>(dispatcher.TryEnqueue([
            lifetime,
            office_session,
            render_session,
            rendered = std::move(rendered),
            generation]() mutable {
            if (generation != lifetime->content_generation_ ||
                office_session != lifetime->office_preview_client_ ||
                render_session != lifetime->pdf_render_client_)
            {
                return;
            }
            try
            {
                lifetime->PdfPageImage().Source(create_pdf_bitmap(rendered));
                lifetime->PdfLoadingOverlay().Visibility(Visibility::Collapsed);
                lifetime->PdfPageText().Text(L"1 / ?");
                lifetime->auto_fit_window_to_content(
                    rendered.page_width_points,
                    rendered.page_height_points);
            }
            catch (const hresult_error&)
            {
                lifetime->show_provider_error(
                    glance::app::localize(L"OfficeConvertError"),
                    generation);
            }
        }));

        auto count = office_session->page_count();
        static_cast<void>(dispatcher.TryEnqueue([
            lifetime,
            office_session,
            render_session,
            count,
            generation] {
            if (generation != lifetime->content_generation_ ||
                office_session != lifetime->office_preview_client_ ||
                render_session != lifetime->pdf_render_client_)
            {
                return;
            }
            lifetime->pdf_page_count_ =
                count.status == glance::contracts::office::Status::success && count.page_count > 0
                    ? count.page_count
                    : 1U;
            lifetime->office_thumbnail_requested_.assign(lifetime->pdf_page_count_, false);
            lifetime->PdfPageText().Text(
                L"1 / " + std::to_wstring(lifetime->pdf_page_count_));
            lifetime->build_pdf_navigation(generation);
        }));
    }

    fire_and_forget MainWindow::render_office_page_async(
        std::uint32_t page_index,
        std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        const auto office_session = office_preview_client_;
        const auto render_session = pdf_render_client_;
        const auto request = pdf_render_request_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (office_session == nullptr || render_session == nullptr || page_index >= pdf_page_count_)
        {
            co_return;
        }
        PdfLoadingText().Visibility(Visibility::Collapsed);
        PdfLoadingOverlay().Visibility(Visibility::Visible);
        const auto width = static_cast<std::uint32_t>(std::clamp(
            std::lround(std::max(512.0, PdfScroller().ActualWidth()) * 2.0),
            1024L,
            4096L));
        const auto height = static_cast<std::uint32_t>(std::clamp(
            std::lround(std::max(512.0, PdfScroller().ActualHeight()) * 2.0),
            1024L,
            4096L));

        co_await resume_background();
        if (request != pdf_render_request_.load(std::memory_order_relaxed))
        {
            co_return;
        }
        auto page = office_session->render_page(page_index);
        if (request != pdf_render_request_.load(std::memory_order_relaxed) ||
            page.status != glance::contracts::office::Status::success)
        {
            co_return;
        }
        auto rendered = render_session->render_emf(page.emf_path, page_index, width, height);
        static_cast<void>(dispatcher.TryEnqueue([
            lifetime,
            office_session,
            render_session,
            rendered = std::move(rendered),
            page_index,
            request,
            generation]() mutable {
            if (generation != lifetime->content_generation_ ||
                office_session != lifetime->office_preview_client_ ||
                render_session != lifetime->pdf_render_client_ ||
                page_index != lifetime->pdf_page_index_ ||
                request != lifetime->pdf_render_request_.load(std::memory_order_relaxed))
            {
                return;
            }
            if (rendered.status != glance::contracts::pdf::Status::success)
            {
                lifetime->show_provider_error(
                    glance::app::localize(L"OfficeConvertError"),
                    generation);
                return;
            }
            try
            {
                lifetime->PdfPageImage().Source(create_pdf_bitmap(rendered));
                lifetime->PdfLoadingOverlay().Visibility(Visibility::Collapsed);
                lifetime->PdfPageText().Text(
                    std::to_wstring(page_index + 1U) + L" / " +
                    std::to_wstring(lifetime->pdf_page_count_));
                lifetime->sync_pdf_thumbnail_selection();
                lifetime->auto_fit_window_to_content(
                    rendered.page_width_points,
                    rendered.page_height_points);
                lifetime->load_office_thumbnail_async(page_index, generation);
            }
            catch (const hresult_error&)
            {
                lifetime->show_provider_error(
                    glance::app::localize(L"OfficeConvertError"),
                    generation);
            }
        }));
    }

    fire_and_forget MainWindow::load_office_thumbnail_async(
        std::uint32_t page_index,
        std::uint64_t generation,
        bool continue_background)
    {
        if (page_index >= office_thumbnail_requested_.size() ||
            office_thumbnail_requested_[page_index])
        {
            co_return;
        }
        office_thumbnail_requested_[page_index] = true;
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        const auto office_session = office_preview_client_;
        const auto render_session = pdf_render_client_;
        if (office_session == nullptr || render_session == nullptr)
        {
            co_return;
        }
        co_await resume_background();
        auto page = office_session->render_page(page_index);
        glance::app::PdfRenderResult rendered;
        if (page.status == glance::contracts::office::Status::success)
        {
            rendered = render_session->render_emf(page.emf_path, page_index, 176, 132);
        }
        static_cast<void>(dispatcher.TryEnqueue([
            lifetime,
            office_session,
            render_session,
            rendered = std::move(rendered),
            page_index,
            generation,
            continue_background]() mutable {
            if (generation != lifetime->content_generation_ ||
                office_session != lifetime->office_preview_client_ ||
                render_session != lifetime->pdf_render_client_ ||
                page_index >= lifetime->office_thumbnail_requested_.size())
            {
                return;
            }
            if (rendered.status != glance::contracts::pdf::Status::success)
            {
                if (continue_background)
                {
                    lifetime->office_thumbnail_background_active_ = false;
                    lifetime->continue_office_thumbnail_generation(generation);
                }
                return;
            }
            try
            {
                if (page_index < lifetime->pdf_thumbnail_images_.size())
                {
                    if (auto image = lifetime->pdf_thumbnail_images_[page_index].get())
                    {
                        image.Source(create_pdf_bitmap(rendered));
                    }
                }
            }
            catch (...)
            {
            }
            if (continue_background)
            {
                lifetime->office_thumbnail_background_active_ = false;
                lifetime->continue_office_thumbnail_generation(generation);
            }
        }));
    }

    void MainWindow::continue_office_thumbnail_generation(std::uint64_t generation)
    {
        if (generation != content_generation_ || !office_emf_preview_ ||
            office_thumbnail_background_active_)
        {
            return;
        }
        for (std::size_t index = 0; index < office_thumbnail_requested_.size(); ++index)
        {
            if (!office_thumbnail_requested_[index])
            {
                office_thumbnail_background_active_ = true;
                load_office_thumbnail_async(
                    static_cast<std::uint32_t>(index),
                    generation,
                    true);
                return;
            }
        }
    }

    fire_and_forget MainWindow::load_office_async(
        std::wstring path,
        std::uint64_t generation,
        std::uint64_t source_size,
        std::uint64_t source_modified_time)
    {
        const auto extension = std::filesystem::path(path).extension().wstring();
        if (_wcsicmp(extension.c_str(), L".doc") == 0 ||
            _wcsicmp(extension.c_str(), L".docx") == 0)
        {
            load_word_emf_async(std::move(path), generation);
            co_return;
        }
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
        const auto operation = std::make_shared<OfficeConversionOperation>();
        office_conversion_ = operation;
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
        const bool staged = !operation->is_cancelled() && !filesystem_error && std::filesystem::copy_file(
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
                operation->attach_process(process.hProcess);
                const DWORD wait_result = WaitForSingleObject(process.hProcess, 120000);
                if (wait_result == WAIT_TIMEOUT)
                {
                    TerminateProcess(process.hProcess, ERROR_TIMEOUT);
                    WaitForSingleObject(process.hProcess, 5000);
                }
                static_cast<void>(GetExitCodeProcess(process.hProcess, &exit_code));
                operation->detach_process(process.hProcess);
                CloseHandle(process.hProcess);
            }
        }
        DeleteFileW(staged_input_path.c_str());

        filesystem_error.clear();
        const bool succeeded = !operation->is_cancelled() && exit_code == 0 &&
            std::filesystem::is_regular_file(output_path, filesystem_error);
        static_cast<void>(dispatcher.TryEnqueue([
            lifetime,
            operation,
            source = std::move(path),
            source_size,
            source_modified_time,
            output = output_path.wstring(),
            generation,
            succeeded] {
            if (lifetime->office_conversion_ == operation)
            {
                lifetime->office_conversion_.reset();
            }
            if (operation->is_cancelled())
            {
                DeleteFileW(output.c_str());
                return;
            }
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
        std::uint64_t generation,
        bool preview_as_text_attempt)
    {
        if (generation != content_generation_)
        {
            return;
        }
        text_chunk_loading_ = false;
        if (!preview.error.empty())
        {
            if (preview_as_text_attempt)
            {
                PreviewAsTextButton().IsEnabled(true);
            }
            else if (current_index_ < files_.size())
            {
                present_generic(files_[current_index_], true, true);
            }
            show_text_preview_error(std::move(preview.error));
            return;
        }

        if (preview_as_text_attempt)
        {
            if (current_index_ >= files_.size())
            {
                return;
            }
            prepare_text_preview(files_[current_index_], false);
        }

        current_text_ = std::move(preview.content);
        current_text_reader_ = std::move(preview.reader);
        current_text_has_more_ = preview.has_more;
        text_chunk_loading_ = false;
        if (current_text_encoding_ == glance::app::TextEncoding::automatic)
        {
            EncodingSelector().Content(box_value(preview.encoding));
        }
        TextEncodingText().Text(L"");

        render_text_content();
        update_line_numbers();
        update_line_number_visibility();
        ensure_text_viewport_filled();
        const bool show_highlight_notice =
            syntax_highlight_notice_pending_ && !syntax_highlighting_;
        syntax_highlight_notice_pending_ = false;
        if (show_highlight_notice)
        {
            show_syntax_highlight_disabled_notice();
        }

        if (markdown)
        {
            render_markdown();
            MarkdownPreviewButton().IsEnabled(!current_text_has_more_);
            set_markdown_preview_mode(current_text_has_more_ ? false : markdown_preview_);
        }
    }

    fire_and_forget MainWindow::load_next_text_chunk_async(std::uint64_t generation)
    {
        if (text_chunk_loading_ || !current_text_has_more_ || current_text_reader_ == nullptr)
        {
            co_return;
        }

        text_chunk_loading_ = true;
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        const auto reader = current_text_reader_;
        co_await resume_background();
        auto preview = glance::app::load_next_text_preview_chunk(reader, text_chunk_bytes);
        static_cast<void>(dispatcher.TryEnqueue(
            [lifetime, preview = std::move(preview), generation]() mutable {
                if (generation != lifetime->content_generation_)
                {
                    return;
                }
                lifetime->text_chunk_loading_ = false;
                if (!preview.error.empty())
                {
                    lifetime->current_text_has_more_ = false;
                    lifetime->current_text_reader_.reset();
                    lifetime->show_text_preview_error(std::move(preview.error));
                    return;
                }

                auto appended = std::move(preview.content);
                lifetime->current_text_reader_ = std::move(preview.reader);
                lifetime->current_text_has_more_ = preview.has_more;
                lifetime->current_text_.append(appended);
                if (lifetime->long_text_mode_)
                {
                    lifetime->append_syntax_ranges(appended);
                    lifetime->append_long_text_content(appended);
                }
                else
                {
                    lifetime->append_text_content(appended);
                }
                lifetime->append_line_numbers(appended);
                lifetime->ensure_text_viewport_filled();
                glance::contracts::log_event(
                    L"Incremental text chunk applied: characters=" +
                    std::to_wstring(appended.size()) +
                    L", has_more=" +
                    std::to_wstring(lifetime->current_text_has_more_));
                if (lifetime->current_text_markdown_)
                {
                    lifetime->render_markdown();
                    lifetime->MarkdownPreviewButton().IsEnabled(!lifetime->current_text_has_more_);
                }
            }));
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
        const bool use_long_text_view = current_text_has_more_ ||
            (current_text_.size() > long_text_render_threshold &&
             current_text_.find(L'\n') != std::wstring::npos);
        long_text_mode_ = use_long_text_view;
        TextPreviewScroller().Visibility(use_long_text_view ? Visibility::Collapsed : Visibility::Visible);
        LongTextList().Visibility(use_long_text_view ? Visibility::Visible : Visibility::Collapsed);
        ScrollViewer::SetHorizontalScrollMode(
            LongTextList(),
            word_wrap_ ? ScrollMode::Disabled : ScrollMode::Enabled);
        ScrollViewer::SetHorizontalScrollBarVisibility(
            LongTextList(),
            word_wrap_ ? ScrollBarVisibility::Disabled : ScrollBarVisibility::Auto);
        if (!use_long_text_view)
        {
            LongTextList().Items().Clear();
            long_text_render_tail_.clear();
            long_text_block_ranges_.clear();
            long_text_block_start_lines_.clear();
            long_text_next_line_ = 1;
        }
        syntax_highlight_state_ = {};
        text_syntax_ranges_.clear();
        text_paragraph_ranges_.clear();
        text_render_tail_.clear();
        text_tail_block_attached_ = false;
        text_highlight_offset_ = 0;
        styled_paragraph_start_ = std::numeric_limits<std::size_t>::max();
        styled_paragraph_end_ = std::numeric_limits<std::size_t>::max();
        syntax_highlight_layout_dirty_ = true;
        if (use_long_text_view)
        {
            rebuild_long_text_content();
            return;
        }
        const double line_height = std::max(18.0, std::ceil(text_preferences_.font_size * 1.38));
        LineNumberText().LineHeight(line_height);
        if (current_text_.empty())
        {
            Paragraph paragraph;
            paragraph.LineHeight(line_height);
            paragraph.LineStackingStrategy(LineStackingStrategy::BlockLineHeight);
            Run run;
            run.Text(L" ");
            paragraph.Inlines().Append(run);
            blocks.Append(paragraph);
        }
        else
        {
            append_text_content(current_text_);
        }
        queue_visible_syntax_highlight_update();
    }

    void MainWindow::append_text_content(std::wstring_view content)
    {
        if (content.empty())
        {
            return;
        }
        const auto blocks = TextContentRichText().Blocks();

        append_syntax_ranges(content);

        if (text_tail_block_attached_ && blocks.Size() > 0)
        {
            blocks.RemoveAtEnd();
            if (!text_paragraph_ranges_.empty())
            {
                text_paragraph_ranges_.pop_back();
            }
            text_tail_block_attached_ = false;
        }

        std::wstring pending = std::move(text_render_tail_);
        pending.append(content);
        const std::size_t pending_source_start = current_text_.size() - pending.size();
        text_render_tail_.clear();
        constexpr std::size_t lines_per_text_block = 128;
        std::size_t segment_start{};
        std::size_t line_count{};
        const double line_height = std::max(18.0, std::ceil(text_preferences_.font_size * 1.38));
        const auto append_paragraph = [this, &blocks, line_height](
                                          std::wstring_view text,
                                          std::size_t source_start,
                                          std::size_t source_length) {
            Paragraph paragraph;
            paragraph.LineHeight(line_height);
            paragraph.LineStackingStrategy(LineStackingStrategy::BlockLineHeight);
            Run run;
            run.Text(hstring(text));
            paragraph.Inlines().Append(run);
            blocks.Append(paragraph);
            text_paragraph_ranges_.push_back({ source_start, source_length });
        };
        for (std::size_t index = 0; index < pending.size(); ++index)
        {
            if (pending[index] != L'\n' || ++line_count < lines_per_text_block)
            {
                continue;
            }
            append_paragraph(
                std::wstring_view(pending).substr(segment_start, index - segment_start),
                pending_source_start + segment_start,
                index - segment_start);
            segment_start = index + 1;
            line_count = 0;
        }
        text_render_tail_.assign(pending, segment_start, std::wstring::npos);
        append_paragraph(
            text_render_tail_.empty() ? std::wstring_view(L" ") : text_render_tail_,
            pending_source_start + segment_start,
            text_render_tail_.size());
        text_tail_block_attached_ = true;
        syntax_highlight_layout_dirty_ = true;
    }

    void MainWindow::append_syntax_ranges(std::wstring_view content)
    {
        if (syntax_highlighting_)
        {
            auto extension = std::filesystem::path(current_text_path_).extension().wstring();
            std::ranges::transform(extension, extension.begin(), [](wchar_t value) {
                return static_cast<wchar_t>(std::towlower(value));
            });
            for (const auto& span : glance::app::highlight_source_chunk(
                     content,
                     extension,
                     syntax_highlight_state_))
            {
                std::optional<std::uint32_t> highlighter_index;
                switch (span.style)
                {
                case glance::app::SyntaxStyle::keyword: highlighter_index = 0; break;
                case glance::app::SyntaxStyle::string: highlighter_index = 1; break;
                case glance::app::SyntaxStyle::comment: highlighter_index = 2; break;
                case glance::app::SyntaxStyle::number: highlighter_index = 3; break;
                case glance::app::SyntaxStyle::directive: highlighter_index = 4; break;
                default: break;
                }
                if (highlighter_index)
                {
                    text_syntax_ranges_.push_back({
                        text_highlight_offset_ + span.start,
                        span.length,
                        span.style });
                }
            }
            text_highlight_offset_ += content.size();
        }
        else
        {
            text_highlight_offset_ += content.size();
        }
    }

    void MainWindow::rebuild_long_text_content()
    {
        LongTextList().Items().Clear();
        long_text_render_tail_.clear();
        long_text_block_ranges_.clear();
        long_text_block_start_lines_.clear();
        long_text_next_line_ = 1;
        syntax_highlight_state_ = {};
        text_syntax_ranges_.clear();
        text_highlight_offset_ = 0;
        append_syntax_ranges(current_text_);
        append_long_text_content(current_text_);
    }

    void MainWindow::append_long_text_content(std::wstring_view content)
    {
        if (content.empty())
        {
            return;
        }
        if (!long_text_block_ranges_.empty())
        {
            long_text_next_line_ = long_text_block_start_lines_.back();
            LongTextList().Items().RemoveAtEnd();
            long_text_block_ranges_.pop_back();
            long_text_block_start_lines_.pop_back();
        }

        std::wstring pending = std::move(long_text_render_tail_);
        pending.append(content);
        const std::size_t pending_source_start = current_text_.size() - pending.size();
        long_text_render_tail_.clear();
        constexpr std::size_t lines_per_long_text_item = 128;
        std::size_t segment_start{};
        std::size_t line_count{};
        const auto append_item = [this, &pending, pending_source_start](
                                     std::size_t start,
                                     std::size_t length,
                                     std::size_t logical_lines) {
            const auto text = std::wstring_view(pending).substr(start, length);
            LongTextList().Items().Append(box_value(hstring(text.empty() ? L" " : text)));
            long_text_block_ranges_.push_back({ pending_source_start + start, length });
            long_text_block_start_lines_.push_back(long_text_next_line_);
            long_text_next_line_ += logical_lines;
        };
        for (std::size_t index = 0; index < pending.size(); ++index)
        {
            if (pending[index] != L'\n' || ++line_count < lines_per_long_text_item)
            {
                continue;
            }
            append_item(segment_start, index - segment_start, lines_per_long_text_item);
            segment_start = index + 1;
            line_count = 0;
        }
        long_text_render_tail_.assign(pending, segment_start, std::wstring::npos);
        append_item(segment_start, long_text_render_tail_.size(), line_count + 1U);
    }

    void MainWindow::queue_visible_syntax_highlight_update()
    {
        const double vertical_offset = TextPreviewScroller().VerticalOffset();
        if (syntax_highlight_update_queued_ ||
            (!syntax_highlight_layout_dirty_ &&
             std::abs(vertical_offset - syntax_highlight_vertical_offset_) < 1.0))
        {
            return;
        }
        syntax_highlight_update_queued_ = true;
        const auto weak = get_weak();
        static_cast<void>(DispatcherQueue().TryEnqueue([weak] {
            if (const auto self = weak.get())
            {
                self->syntax_highlight_update_queued_ = false;
                try
                {
                    self->update_visible_syntax_highlights();
                }
                catch (const hresult_error& error)
                {
                    self->updating_syntax_highlights_ = false;
                    glance::contracts::log_event(
                        L"Visible syntax highlight update failed: " +
                        std::wstring(error.message()));
                }
                catch (const std::exception& error)
                {
                    self->updating_syntax_highlights_ = false;
                    glance::contracts::log_event(
                        L"Visible syntax highlight update failed: " +
                        to_hstring(error.what()));
                }
                catch (...)
                {
                    self->updating_syntax_highlights_ = false;
                    glance::contracts::log_event(L"Visible syntax highlight update failed.");
                }
            }
        }));
    }

    void MainWindow::update_visible_syntax_highlights()
    {
        if (updating_syntax_highlights_)
        {
            return;
        }
        updating_syntax_highlights_ = true;
        const auto blocks = TextContentRichText().Blocks();
        const auto set_plain_paragraph = [this, &blocks](std::size_t index) {
            if (index >= blocks.Size() || index >= text_paragraph_ranges_.size())
            {
                return;
            }
            const auto paragraph = blocks.GetAt(static_cast<std::uint32_t>(index)).try_as<Paragraph>();
            if (paragraph == nullptr)
            {
                return;
            }
            const auto& range = text_paragraph_ranges_[index];
            paragraph.Inlines().Clear();
            Run run;
            run.Text(hstring(range.length == 0
                ? std::wstring_view(L" ")
                : std::wstring_view(current_text_).substr(range.start, range.length)));
            paragraph.Inlines().Append(run);
        };
        if (styled_paragraph_start_ != std::numeric_limits<std::size_t>::max())
        {
            for (std::size_t index = styled_paragraph_start_;
                 index < styled_paragraph_end_ && index < text_paragraph_ranges_.size();
                 ++index)
            {
                set_plain_paragraph(index);
            }
        }
        styled_paragraph_start_ = std::numeric_limits<std::size_t>::max();
        styled_paragraph_end_ = std::numeric_limits<std::size_t>::max();

        if (!syntax_highlighting_ || text_syntax_ranges_.empty() ||
            current_text_.empty() || text_paragraph_ranges_.empty())
        {
            syntax_highlight_layout_dirty_ = false;
            syntax_highlight_vertical_offset_ = TextPreviewScroller().VerticalOffset();
            updating_syntax_highlights_ = false;
            return;
        }

        const double extent_height = TextPreviewScroller().ExtentHeight();
        const double viewport_height = TextPreviewScroller().ViewportHeight();
        const double vertical_offset = TextPreviewScroller().VerticalOffset();
        constexpr std::size_t highlight_buffer_characters = 512U;
        const bool measured_scroll_extent = extent_height > viewport_height && extent_height > 0.0;
        const double start_ratio = measured_scroll_extent
            ? std::clamp(vertical_offset / extent_height, 0.0, 1.0)
            : 0.0;
        const double end_ratio = measured_scroll_extent
            ? std::clamp((vertical_offset + viewport_height) / extent_height, 0.0, 1.0)
            : 0.0;
        const auto approximate_start = static_cast<std::size_t>(current_text_.size() * start_ratio);
        const auto approximate_end = measured_scroll_extent
            ? static_cast<std::size_t>(current_text_.size() * end_ratio)
            : std::min(current_text_.size(), highlight_buffer_characters);
        const std::size_t visible_start = approximate_start > highlight_buffer_characters
            ? approximate_start - highlight_buffer_characters
            : 0;
        const std::size_t visible_end = std::min(
            current_text_.size(),
            approximate_end + highlight_buffer_characters);

        const auto first_paragraph = std::lower_bound(
            text_paragraph_ranges_.begin(),
            text_paragraph_ranges_.end(),
            visible_start,
            [](const TextParagraphRange& range, std::size_t position) {
                return range.start + range.length < position;
            });
        const auto last_paragraph = std::lower_bound(
            first_paragraph,
            text_paragraph_ranges_.end(),
            visible_end,
            [](const TextParagraphRange& range, std::size_t position) {
                return range.start < position;
            });
        const std::size_t paragraph_start = static_cast<std::size_t>(std::distance(
            text_paragraph_ranges_.begin(),
            first_paragraph));
        const std::size_t paragraph_end = std::min(
            text_paragraph_ranges_.size(),
            static_cast<std::size_t>(std::distance(
                text_paragraph_ranges_.begin(),
                last_paragraph)) + 1U);
        const bool dark = RootGrid().ActualTheme() == ElementTheme::Dark;
        for (std::size_t paragraph_index = paragraph_start;
             paragraph_index < paragraph_end && paragraph_index < blocks.Size();
             ++paragraph_index)
        {
            const auto paragraph = blocks.GetAt(
                static_cast<std::uint32_t>(paragraph_index)).try_as<Paragraph>();
            if (paragraph == nullptr)
            {
                continue;
            }
            const auto& paragraph_range = text_paragraph_ranges_[paragraph_index];
            const std::size_t paragraph_end_offset = paragraph_range.start + paragraph_range.length;
            paragraph.Inlines().Clear();
            std::size_t cursor = paragraph_range.start;
            auto syntax_range = std::lower_bound(
                text_syntax_ranges_.begin(),
                text_syntax_ranges_.end(),
                paragraph_range.start,
                [](const TextSyntaxRange& range, std::size_t position) {
                    return range.start + range.length <= position;
                });
            const auto append_run = [&paragraph](
                                        std::wstring_view text,
                                        Media::Brush foreground = nullptr) {
                if (text.empty())
                {
                    return;
                }
                Run run;
                run.Text(hstring(text));
                if (foreground != nullptr)
                {
                    run.Foreground(foreground);
                }
                paragraph.Inlines().Append(run);
            };
            while (syntax_range != text_syntax_ranges_.end() &&
                   syntax_range->start < paragraph_end_offset)
            {
                const std::size_t style_start = std::max(cursor, syntax_range->start);
                const std::size_t style_end = std::min(
                    paragraph_end_offset,
                    syntax_range->start + syntax_range->length);
                if (style_start > cursor)
                {
                    append_run(std::wstring_view(current_text_).substr(
                        cursor,
                        style_start - cursor));
                }
                if (style_end > style_start)
                {
                    append_run(
                        std::wstring_view(current_text_).substr(
                            style_start,
                            style_end - style_start),
                        syntax_brush(
                            syntax_range->style,
                            text_preferences_.syntax_theme,
                            dark));
                    cursor = style_end;
                }
                ++syntax_range;
            }
            if (cursor < paragraph_end_offset)
            {
                append_run(std::wstring_view(current_text_).substr(
                    cursor,
                    paragraph_end_offset - cursor));
            }
            if (paragraph.Inlines().Size() == 0)
            {
                append_run(L" ");
            }
        }
        styled_paragraph_start_ = paragraph_start;
        styled_paragraph_end_ = paragraph_end;
        syntax_highlight_layout_dirty_ = false;
        syntax_highlight_vertical_offset_ = TextPreviewScroller().VerticalOffset();
        updating_syntax_highlights_ = false;
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
        for (std::uint32_t index = 0; index < blocks.Size(); ++index)
        {
            if (const auto paragraph = blocks.GetAt(index).try_as<Paragraph>())
            {
                paragraph.LineHeight(line_height);
                paragraph.LineStackingStrategy(LineStackingStrategy::BlockLineHeight);
            }
        }
    }

    void MainWindow::update_text_layout()
    {
        const bool should_use_long_text_view = current_text_has_more_ ||
            (current_text_.size() > long_text_render_threshold &&
             current_text_.find(L'\n') != std::wstring::npos);
        if (should_use_long_text_view != long_text_mode_ && !current_text_.empty())
        {
            render_text_content();
        }
        TextContentRichText().TextWrapping(word_wrap_ ? TextWrapping::Wrap : TextWrapping::NoWrap);
        TextPreviewScroller().HorizontalScrollMode(
            word_wrap_ ? ScrollMode::Disabled : ScrollMode::Enabled);
        TextPreviewScroller().HorizontalScrollBarVisibility(
            word_wrap_ ? ScrollBarVisibility::Disabled : ScrollBarVisibility::Auto);
        ScrollViewer::SetHorizontalScrollMode(
            LongTextList(),
            word_wrap_ ? ScrollMode::Disabled : ScrollMode::Enabled);
        ScrollViewer::SetHorizontalScrollBarVisibility(
            LongTextList(),
            word_wrap_ ? ScrollBarVisibility::Disabled : ScrollBarVisibility::Auto);
        for (std::uint32_t index = 0; index < LongTextList().Items().Size(); ++index)
        {
            const auto container = LongTextList().ContainerFromIndex(index).try_as<ListViewItem>();
            const auto root = container != nullptr
                ? container.ContentTemplateRoot().try_as<Grid>()
                : nullptr;
            if (root != nullptr && root.Children().Size() >= 2)
            {
                if (const auto content = root.Children().GetAt(1).try_as<TextBlock>())
                {
                    content.TextWrapping(word_wrap_ ? TextWrapping::Wrap : TextWrapping::NoWrap);
                }
            }
        }
        TextContentRichText().Width(
            word_wrap_
                ? std::max(1.0, TextPreviewScroller().ActualWidth() - LineNumberGutter().ActualWidth())
                : std::numeric_limits<double>::quiet_NaN());
        update_line_numbers();
        update_line_number_visibility();
        ensure_text_viewport_filled();
    }

    void MainWindow::update_line_numbers()
    {
        if (current_text_.empty())
        {
            set_line_number_text(L"");
            text_line_count_ = 0;
            line_numbers_simple_ = false;
            virtual_line_numbers_ = false;
            return;
        }

        const auto simple_numbers = [this] {
            std::wostringstream output;
            for (std::size_t line = 1; line <= text_line_count_; ++line)
            {
                if (line > 1)
                {
                    output << L'\n';
                }
                output << line;
            }
            return output.str();
        };
        text_line_count_ = 1
            + static_cast<std::size_t>(std::ranges::count(current_text_, L'\n'));

        if (current_text_has_more_ || current_text_.size() > text_chunk_bytes)
        {
            line_numbers_simple_ = true;
            virtual_line_numbers_ = true;
            update_virtual_line_numbers();
            return;
        }
        virtual_line_numbers_ = false;
        LineNumberText().Margin(Thickness{});
        if (!word_wrap_)
        {
            line_numbers_simple_ = true;
            set_line_number_text(simple_numbers());
            return;
        }

        const double content_width = TextContentRichText().Width() - 32.0;
        if (!std::isfinite(content_width) || content_width <= 1.0)
        {
            line_numbers_simple_ = true;
            set_line_number_text(simple_numbers());
            return;
        }

        com_ptr<IDWriteFactory> factory;
        if (FAILED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(factory.put()))))
        {
            line_numbers_simple_ = true;
            set_line_number_text(simple_numbers());
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
            line_numbers_simple_ = true;
            set_line_number_text(simple_numbers());
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
            line_numbers_simple_ = true;
            set_line_number_text(simple_numbers());
            return;
        }

        UINT32 metric_count{};
        layout->GetLineMetrics(nullptr, 0, &metric_count);
        std::vector<DWRITE_LINE_METRICS> metrics(metric_count);
        if (FAILED(layout->GetLineMetrics(metrics.data(), metric_count, &metric_count)))
        {
            line_numbers_simple_ = true;
            set_line_number_text(simple_numbers());
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
        line_numbers_simple_ = false;
        set_line_number_text(output.str());
    }

    void MainWindow::set_line_number_text(std::wstring text)
    {
        line_number_text_ = std::move(text);
        const auto blocks = LineNumberText().Blocks();
        blocks.Clear();
        if (line_number_text_.empty())
        {
            return;
        }

        constexpr std::size_t lines_per_number_block = 64;
        const double line_height = std::max(18.0, std::ceil(text_preferences_.font_size * 1.38));
        std::size_t segment_start{};
        std::size_t line_count{};
        const auto append_block = [&blocks, line_height](std::wstring_view value) {
            Paragraph paragraph;
            paragraph.LineHeight(line_height);
            paragraph.LineStackingStrategy(LineStackingStrategy::BlockLineHeight);
            Run run;
            run.Text(hstring(value));
            paragraph.Inlines().Append(run);
            blocks.Append(paragraph);
        };
        for (std::size_t index = 0; index < line_number_text_.size(); ++index)
        {
            if (line_number_text_[index] != L'\n' || ++line_count < lines_per_number_block)
            {
                continue;
            }
            append_block(std::wstring_view(line_number_text_).substr(
                segment_start,
                index - segment_start));
            segment_start = index + 1;
            line_count = 0;
        }
        append_block(std::wstring_view(line_number_text_).substr(segment_start));
    }

    void MainWindow::append_line_numbers(std::wstring_view content)
    {
        const auto added_lines = static_cast<std::size_t>(std::ranges::count(content, L'\n'));
        const bool should_use_simple =
            !word_wrap_ || current_text_has_more_ || current_text_.size() > text_chunk_bytes;
        if (!line_numbers_simple_ || !should_use_simple || text_line_count_ == 0)
        {
            update_line_numbers();
            return;
        }

        if (added_lines == 0)
        {
            return;
        }
        if (virtual_line_numbers_)
        {
            text_line_count_ += added_lines;
            update_virtual_line_numbers();
            return;
        }
        std::wstring numbers = line_number_text_;
        for (std::size_t index = 0; index < added_lines; ++index)
        {
            numbers.push_back(L'\n');
            numbers.append(std::to_wstring(++text_line_count_));
        }
        set_line_number_text(std::move(numbers));
    }

    void MainWindow::update_virtual_line_numbers()
    {
        if (!virtual_line_numbers_ || text_line_count_ == 0)
        {
            return;
        }
        const double line_height = std::max(18.0, std::ceil(text_preferences_.font_size * 1.38));
        const std::size_t first_line = std::min(
            text_line_count_ - 1,
            static_cast<std::size_t>(std::max(0.0, TextPreviewScroller().VerticalOffset()) / line_height));
        const std::size_t visible_line_count = static_cast<std::size_t>(std::ceil(
            std::max(line_height, TextPreviewScroller().ViewportHeight()) / line_height)) + 4U;
        const std::size_t last_line = std::min(
            text_line_count_,
            first_line + visible_line_count);
        std::wostringstream output;
        for (std::size_t line = first_line + 1; line <= last_line; ++line)
        {
            if (line > first_line + 1)
            {
                output << L'\n';
            }
            output << line;
        }
        set_line_number_text(output.str());
        LineNumberText().Margin(Thickness{ 0.0, first_line * line_height, 0.0, 0.0 });
    }

    void MainWindow::ensure_text_viewport_filled()
    {
        if (!long_text_mode_ || !current_text_has_more_ || text_chunk_loading_)
        {
            return;
        }
        const double viewport_height = LongTextList().ActualHeight();
        if (viewport_height <= 0.0)
        {
            return;
        }
        const double line_height = std::max(18.0, std::ceil(text_preferences_.font_size * 1.38));
        const std::size_t minimum_lines =
            static_cast<std::size_t>(std::ceil(viewport_height / line_height)) + 4U;
        if (text_line_count_ < minimum_lines)
        {
            load_next_text_chunk_async(content_generation_);
        }
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
        if (kind != glance::app::PreviewKind::generic)
        {
            GenericAdvancedInfoButton().Visibility(Visibility::Collapsed);
        }
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
            cancel_pdf_render();
            PdfPageImage().Source(nullptr);
            hide_password_prompt();
        }
    }

    void MainWindow::dismiss_preview_info_bar()
    {
        syntax_highlight_notice_pending_ = false;
        preview_notice_active_ = false;
        if (preview_notice_timer_ != nullptr)
        {
            preview_notice_timer_.Stop();
        }
        PreviewErrorInfoBar().IsOpen(false);
    }

    void MainWindow::show_syntax_highlight_disabled_notice()
    {
        if (preview_notice_timer_ != nullptr)
        {
            preview_notice_timer_.Stop();
        }
        PreviewErrorInfoBar().IsOpen(false);
        preview_notice_active_ = true;
        PreviewErrorInfoBar().Title(L"");
        PreviewErrorInfoBar().Message(
            glance::app::localize(L"SyntaxHighlightDisabledLargeFile"));
        PreviewErrorInfoBar().Severity(InfoBarSeverity::Informational);
        PreviewErrorInfoBar().IsClosable(false);
        PreviewErrorInfoBar().IsOpen(true);
        if (preview_notice_timer_ == nullptr)
        {
            preview_notice_timer_ = DispatcherTimer();
            preview_notice_timer_.Interval(std::chrono::seconds(3));
            const auto weak = get_weak();
            preview_notice_timer_.Tick([weak](IInspectable const&, IInspectable const&) {
                if (const auto self = weak.get())
                {
                    self->preview_notice_timer_.Stop();
                    self->preview_notice_active_ = false;
                    self->PreviewErrorInfoBar().IsOpen(false);
                }
            });
        }
        preview_notice_timer_.Start();
    }

    void MainWindow::show_text_preview_error(std::wstring message)
    {
        dismiss_preview_info_bar();
        PreviewErrorInfoBar().Title(glance::app::localize(L"PreviewErrorInfoBar.Title"));
        PreviewErrorInfoBar().Message(std::move(message));
        PreviewErrorInfoBar().Severity(InfoBarSeverity::Error);
        PreviewErrorInfoBar().IsClosable(true);
        PreviewErrorInfoBar().IsOpen(true);
    }

    void MainWindow::update_preview_mode_button()
    {
        const bool available =
            content_preview_kind_ != glance::app::PreviewKind::generic &&
            current_index_ < files_.size() &&
            !files_[current_index_].path.empty() &&
            !files_[current_index_].is_cloud_placeholder &&
            (files_[current_index_].attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
        PreviewModeButton().Visibility(available ? Visibility::Visible : Visibility::Collapsed);
        PreviewModeButton().IsChecked(available && basic_info_mode_);
        ToolTipService::SetToolTip(
            PreviewModeButton(),
            box_value(glance::app::localize(
                available && basic_info_mode_
                    ? L"PreviewModeShowContentTooltip"
                    : L"PreviewModeShowInfoTooltip")));
    }

    void MainWindow::update_preview_as_text_button()
    {
        const bool available =
            generic_text_preview_allowed_ &&
            !basic_info_mode_ &&
            current_kind_ == glance::app::PreviewKind::generic &&
            current_index_ < files_.size() &&
            !files_[current_index_].path.empty() &&
            !files_[current_index_].is_cloud_placeholder &&
            (files_[current_index_].attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
            files_[current_index_].size <= maximum_preview_as_text_bytes &&
            glance::app::can_try_preview_as_text(files_[current_index_].path);
        PreviewAsTextButton().Visibility(available ? Visibility::Visible : Visibility::Collapsed);
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
        const double viewport_width = std::max(1.0, ImagePanel().ActualWidth());
        const double viewport_height = std::max(1.0, ImagePanel().ActualHeight());
        ImageFitSurface().Width(viewport_width);
        ImageFitSurface().Height(viewport_height);

        if (image_pixel_width_ == 0 || image_pixel_height_ == 0)
        {
            return;
        }

        const bool swaps_dimensions =
            static_cast<int>(std::lround(image_rotation_)) % 180 != 0;
        const double rotated_width = swaps_dimensions ? image_pixel_height_ : image_pixel_width_;
        const double rotated_height = swaps_dimensions ? image_pixel_width_ : image_pixel_height_;
        const double fit_scale = std::min(
            viewport_width / rotated_width,
            viewport_height / rotated_height);
        ImagePreview().Width(std::max(1.0, image_pixel_width_ * fit_scale));
        ImagePreview().Height(std::max(1.0, image_pixel_height_ * fit_scale));
    }

    void MainWindow::fit_image_to_viewport()
    {
        image_panning_ = false;
        update_image_fit_surface();
        static_cast<void>(ImageScroller().ChangeView(0.0, 0.0, 1.0F, true));
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

    void MainWindow::PreviewModeButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (content_preview_kind_ == glance::app::PreviewKind::generic ||
            current_index_ >= files_.size())
        {
            basic_info_mode_ = false;
            update_preview_mode_button();
            return;
        }

        const auto& file = files_[current_index_];
        if (file.path.empty() || file.is_cloud_placeholder ||
            (file.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            basic_info_mode_ = false;
            update_preview_mode_button();
            return;
        }

        basic_info_mode_ = PreviewModeButton().IsChecked().Value();
        update_preview_mode_button();
        if (!basic_info_mode_)
        {
            present_file(current_index_);
            return;
        }

        cancel_office_conversion();
        ++content_generation_;
        dismiss_preview_info_bar();
        present_generic(file, false, true);
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
        TextPreviewScroller().Visibility(
            !preview && !long_text_mode_ ? Visibility::Visible : Visibility::Collapsed);
        LongTextList().Visibility(
            !preview && long_text_mode_ ? Visibility::Visible : Visibility::Collapsed);
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
        LineNumberGutter().Visibility(
            line_numbers_visible_ && !long_text_mode_ ? Visibility::Visible : Visibility::Collapsed);
        if (long_text_mode_)
        {
            for (std::uint32_t index = 0; index < LongTextList().Items().Size(); ++index)
            {
                const auto container = LongTextList().ContainerFromIndex(index).try_as<ListViewItem>();
                const auto root = container != nullptr
                    ? container.ContentTemplateRoot().try_as<Grid>()
                    : nullptr;
                if (root != nullptr && root.Children().Size() > 0)
                {
                    if (const auto border = root.Children().GetAt(0).try_as<Border>())
                    {
                        border.Visibility(
                            line_numbers_visible_ ? Visibility::Visible : Visibility::Collapsed);
                    }
                }
            }
        }
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
        if (syntax_highlight_notice_pending_ || preview_notice_active_)
        {
            dismiss_preview_info_bar();
        }
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

    void MainWindow::ArchiveHeaderButton_Click(IInspectable const& sender, RoutedEventArgs const&)
    {
        if (!archive_preview_is_directory_)
        {
            return;
        }

        const auto button = sender.try_as<Button>();
        const auto tag = button != nullptr
            ? unbox_value_or<hstring>(button.Tag(), L"")
            : hstring{};
        auto field = glance::app::FolderSortField::name;
        if (tag == L"type")
        {
            field = glance::app::FolderSortField::type;
        }
        else if (tag == L"modified")
        {
            field = glance::app::FolderSortField::modified_time;
        }
        else if (tag == L"size")
        {
            field = glance::app::FolderSortField::size;
        }

        if (folder_preview_preferences_.sort_field == field)
        {
            folder_preview_preferences_.ascending = !folder_preview_preferences_.ascending;
        }
        else
        {
            folder_preview_preferences_.sort_field = field;
            folder_preview_preferences_.ascending = true;
        }
        glance::app::save_folder_preview_preferences(folder_preview_preferences_);
        update_archive_header_state();

        if (archive_render_state_ == nullptr)
        {
            return;
        }

        auto preview = std::move(archive_render_state_->preview);
        archive_render_state_.reset();
        ArchiveEntryTree().RootNodes().Clear();
        FolderEntryList().Items().Clear();
        ArchiveStatusText().Text(glance::app::localize(L"LoadingFolder"));
        apply_archive_preview(std::move(preview), content_generation_);
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
        set_line_number_text(L"");
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

    void MainWindow::TextPreviewScroller_ViewChanged(
        IInspectable const&,
        ScrollViewerViewChangedEventArgs const&)
    {
        update_virtual_line_numbers();
        queue_visible_syntax_highlight_update();
        const double scrollable_height = TextPreviewScroller().ScrollableHeight();
        if (scrollable_height <= 0.0 ||
            TextPreviewScroller().VerticalOffset() < scrollable_height * 0.75)
        {
            return;
        }
        load_next_text_chunk_async(content_generation_);
    }

    void MainWindow::LongTextList_SizeChanged(IInspectable const&, SizeChangedEventArgs const&)
    {
        ensure_text_viewport_filled();
    }

    void MainWindow::LongTextList_ContainerContentChanging(
        ListViewBase const&,
        ContainerContentChangingEventArgs const& args)
    {
        if (args.InRecycleQueue())
        {
            return;
        }
        if (args.Phase() == 0)
        {
            const auto weak = get_weak();
            args.RegisterUpdateCallback(
                [weak](
                    ListViewBase const& callback_sender,
                    ContainerContentChangingEventArgs const& callback_args) {
                    if (const auto self = weak.get())
                    {
                        self->LongTextList_ContainerContentChanging(
                            callback_sender,
                            callback_args);
                    }
                });
            return;
        }
        const std::size_t item_index = args.ItemIndex();
        if (item_index >= long_text_block_ranges_.size())
        {
            return;
        }
        const auto root = args.ItemContainer().ContentTemplateRoot().try_as<Grid>();
        if (root == nullptr || root.Children().Size() < 2)
        {
            return;
        }
        const auto line_border = root.Children().GetAt(0).try_as<Border>();
        const auto line_numbers = line_border != nullptr
            ? line_border.Child().try_as<TextBlock>()
            : nullptr;
        const auto content = root.Children().GetAt(1).try_as<TextBlock>();
        if (line_numbers == nullptr || content == nullptr)
        {
            return;
        }

        const auto& block_range = long_text_block_ranges_[item_index];
        const auto block_text = std::wstring_view(current_text_).substr(
            block_range.start,
            block_range.length);
        content.Text(hstring(block_text.empty() ? L" " : block_text));
        content.FontFamily(Media::FontFamily(text_preferences_.font_family));
        content.FontSize(text_preferences_.font_size);
        content.TextWrapping(word_wrap_ ? TextWrapping::Wrap : TextWrapping::NoWrap);

        std::wostringstream numbers;
        std::size_t line = long_text_block_start_lines_[item_index];
        numbers << line;
        for (const wchar_t character : block_text)
        {
            if (character == L'\n')
            {
                numbers << L'\n' << ++line;
            }
        }
        line_numbers.Text(numbers.str());
        line_numbers.FontFamily(Media::FontFamily(text_preferences_.font_family));
        line_numbers.FontSize(text_preferences_.font_size);
        const double line_height = std::max(18.0, std::ceil(text_preferences_.font_size * 1.38));
        line_numbers.LineHeight(line_height);
        line_numbers.LineStackingStrategy(LineStackingStrategy::BlockLineHeight);
        content.LineHeight(line_height);
        content.LineStackingStrategy(LineStackingStrategy::BlockLineHeight);
        line_border.Visibility(line_numbers_visible_ ? Visibility::Visible : Visibility::Collapsed);

        const auto highlighters = content.TextHighlighters();
        highlighters.Clear();
        if (syntax_highlighting_)
        {
            const bool dark = RootGrid().ActualTheme() == ElementTheme::Dark;
            std::array<TextHighlighter, 5> item_highlighters;
            const std::array styles{
                glance::app::SyntaxStyle::keyword,
                glance::app::SyntaxStyle::string,
                glance::app::SyntaxStyle::comment,
                glance::app::SyntaxStyle::number,
                glance::app::SyntaxStyle::directive };
            const auto transparent_background = Media::SolidColorBrush(
                Windows::UI::Color{ 0, 0, 0, 0 });
            for (std::size_t index = 0; index < styles.size(); ++index)
            {
                item_highlighters[index] = TextHighlighter();
                item_highlighters[index].Foreground(syntax_brush(
                    styles[index],
                    text_preferences_.syntax_theme,
                    dark));
                item_highlighters[index].Background(transparent_background);
            }
            const std::size_t block_end = block_range.start + block_range.length;
            auto range = std::lower_bound(
                text_syntax_ranges_.begin(),
                text_syntax_ranges_.end(),
                block_range.start,
                [](const TextSyntaxRange& value, std::size_t position) {
                    return value.start + value.length <= position;
                });
            while (range != text_syntax_ranges_.end() && range->start < block_end)
            {
                const std::size_t start = std::max(range->start, block_range.start);
                const std::size_t end = std::min(range->start + range->length, block_end);
                std::size_t highlighter_index{};
                switch (range->style)
                {
                case glance::app::SyntaxStyle::keyword: highlighter_index = 0; break;
                case glance::app::SyntaxStyle::string: highlighter_index = 1; break;
                case glance::app::SyntaxStyle::comment: highlighter_index = 2; break;
                case glance::app::SyntaxStyle::number: highlighter_index = 3; break;
                case glance::app::SyntaxStyle::directive: highlighter_index = 4; break;
                default: ++range; continue;
                }
                TextRange text_range;
                text_range.StartIndex = static_cast<std::int32_t>(start - block_range.start);
                text_range.Length = static_cast<std::int32_t>(end - start);
                item_highlighters[highlighter_index].Ranges().Append(text_range);
                ++range;
            }
            for (const auto& highlighter : item_highlighters)
            {
                highlighters.Append(highlighter);
            }
        }

        const auto item_count = LongTextList().Items().Size();
        if (current_text_has_more_ && item_count > 0 &&
            (item_index + 1U) * 4U >= item_count * 3U)
        {
            load_next_text_chunk_async(content_generation_);
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

        const double font_size = std::clamp(text_preferences_.font_size + steps, 7.0, 32.0);
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
        fit_image_to_viewport();
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

    void MainWindow::MediaPanel_PointerWheelChanged(
        IInspectable const&,
        PointerRoutedEventArgs const& args)
    {
        if (current_kind_ != glance::app::PreviewKind::media ||
            MediaPreview().MediaPlayer() == nullptr)
        {
            return;
        }

        const int delta = args.GetCurrentPoint(MediaPanel()).Properties().MouseWheelDelta();
        if (delta == 0)
        {
            return;
        }

        args.Handled(true);
        const auto player = MediaPreview().MediaPlayer();
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
        {
            media_volume_wheel_delta_ += delta;
            const int steps = media_volume_wheel_delta_ / WHEEL_DELTA;
            media_volume_wheel_delta_ %= WHEEL_DELTA;
            if (steps != 0)
            {
                const double volume = std::clamp(
                    MediaVolumeSlider().Value() + steps * 5.0,
                    0.0,
                    100.0);
                MediaVolumeSlider().Value(volume);
                if (volume > 0.0)
                {
                    player.IsMuted(false);
                }
            }
        }
        else
        {
            media_seek_wheel_delta_ += delta;
            const int steps = media_seek_wheel_delta_ / WHEEL_DELTA;
            media_seek_wheel_delta_ %= WHEEL_DELTA;
            if (steps != 0)
            {
                const auto session = player.PlaybackSession();
                const double duration = std::max(
                    0.0,
                    session.NaturalDuration().count() / 10000000.0);
                if (duration > 0.0)
                {
                    const double position = std::clamp(
                        session.Position().count() / 10000000.0 + steps * 5.0,
                        0.0,
                        duration);
                    session.Position(std::chrono::duration_cast<Windows::Foundation::TimeSpan>(
                        std::chrono::duration<double>(position)));
                    updating_media_position_ = true;
                    MediaSeekSlider().Value(position);
                    updating_media_position_ = false;
                }
            }
        }
        show_media_controls();
        update_media_controls();
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
        if (pdf_render_client_ == nullptr || pdf_page_index_ == 0)
        {
            return;
        }
        navigate_to_pdf_page(pdf_page_index_ - 1);
    }

    void MainWindow::NextPdfPageButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (pdf_render_client_ == nullptr || pdf_page_index_ + 1 >= pdf_page_count_)
        {
            return;
        }
        navigate_to_pdf_page(pdf_page_index_ + 1);
    }

    void MainWindow::PdfThumbnailsButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        show_pdf_navigation(true);
    }

    void MainWindow::PdfOutlineButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        show_pdf_navigation(false);
    }

    void MainWindow::PdfThumbnailList_SelectionChanged(
        IInspectable const&,
        SelectionChangedEventArgs const&)
    {
        if (pdf_thumbnail_selection_updating_ || PdfThumbnailList().SelectedIndex() < 0)
        {
            return;
        }
        navigate_to_pdf_page(static_cast<std::uint32_t>(PdfThumbnailList().SelectedIndex()));
    }

    void MainWindow::PdfOutlineEntry_Click(IInspectable const& sender, RoutedEventArgs const&)
    {
        const auto button = sender.try_as<Button>();
        if (button == nullptr || button.Tag() == nullptr)
        {
            return;
        }
        navigate_to_pdf_page(unbox_value<std::uint32_t>(button.Tag()));
    }

    void MainWindow::PasswordPromptSubmitButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        submit_password();
    }

    void MainWindow::PasswordPromptInput_KeyDown(IInspectable const&, KeyRoutedEventArgs const& args)
    {
        if (args.Key() == Windows::System::VirtualKey::Enter)
        {
            args.Handled(true);
            submit_password();
        }
    }

    void MainWindow::show_pdf_navigation(bool thumbnails)
    {
        const bool show_outline = !thumbnails && !pdf_outline_.empty();
        PdfThumbnailsButton().IsChecked(!show_outline);
        PdfOutlineButton().IsChecked(show_outline);
        PdfThumbnailList().Visibility(show_outline ? Visibility::Collapsed : Visibility::Visible);
        PdfOutlineTree().Visibility(show_outline ? Visibility::Visible : Visibility::Collapsed);
        if (!show_outline)
        {
            sync_pdf_thumbnail_selection();
        }
    }

    void MainWindow::sync_pdf_thumbnail_selection()
    {
        const auto items = PdfThumbnailList().Items();
        if (pdf_page_index_ >= items.Size())
        {
            return;
        }
        pdf_thumbnail_selection_updating_ = true;
        PdfThumbnailList().SelectedIndex(static_cast<std::int32_t>(pdf_page_index_));
        pdf_thumbnail_selection_updating_ = false;
        PdfThumbnailList().ScrollIntoView(items.GetAt(pdf_page_index_));
    }

    void MainWindow::build_pdf_navigation(std::uint64_t generation)
    {
        pdf_thumbnail_selection_updating_ = true;
        PdfThumbnailList().Items().Clear();
        PdfOutlineTree().RootNodes().Clear();
        pdf_thumbnail_images_.clear();
        pdf_thumbnail_images_.reserve(pdf_page_count_);
        pdf_thumbnail_items_built_ = 0;
        pdf_thumbnail_selection_updating_ = false;

        std::vector<TreeViewNode> parents;
        for (const auto& outline : pdf_outline_)
        {
            Button button;
            button.HorizontalAlignment(HorizontalAlignment::Stretch);
            button.HorizontalContentAlignment(HorizontalAlignment::Left);
            button.Background(nullptr);
            button.BorderThickness(Thickness{ 0 });
            button.Padding(Thickness{ 4, 2, 4, 2 });
            button.IsEnabled(outline.page_index >= 0 &&
                static_cast<std::uint32_t>(outline.page_index) < pdf_page_count_);
            if (button.IsEnabled())
            {
                button.Tag(box_value(static_cast<std::uint32_t>(outline.page_index)));
                button.Click({ this, &MainWindow::PdfOutlineEntry_Click });
            }
            TextBlock text;
            text.Text(outline.title);
            text.TextTrimming(TextTrimming::CharacterEllipsis);
            button.Content(text);
            TreeViewNode node;
            node.Content(button);
            const std::size_t depth = std::min<std::size_t>(outline.depth, parents.size());
            if (depth == 0)
            {
                PdfOutlineTree().RootNodes().Append(node);
            }
            else
            {
                parents[depth - 1].Children().Append(node);
            }
            if (parents.size() > depth)
            {
                parents.resize(depth);
            }
            parents.push_back(node);
        }
        PdfOutlineButton().IsEnabled(!pdf_outline_.empty());
        show_pdf_navigation(true);
        append_pdf_thumbnail_batch(generation);
    }

    void MainWindow::append_pdf_thumbnail_batch(std::uint64_t generation)
    {
        if (generation != content_generation_ || pdf_render_client_ == nullptr)
        {
            return;
        }
        constexpr std::uint32_t batch_size = 64;
        const auto end = pdf_thumbnail_items_built_ +
            std::min(batch_size, pdf_page_count_ - pdf_thumbnail_items_built_);
        for (std::uint32_t page = pdf_thumbnail_items_built_; page < end; ++page)
        {
            StackPanel content;
            content.Spacing(4);
            Grid preview;
            FontIcon placeholder;
            placeholder.Glyph(L"\xE8A5");
            placeholder.FontSize(32);
            placeholder.Opacity(0.35);
            placeholder.HorizontalAlignment(HorizontalAlignment::Center);
            placeholder.VerticalAlignment(VerticalAlignment::Center);
            Image image;
            image.Width(176);
            image.Height(132);
            image.Stretch(Microsoft::UI::Xaml::Media::Stretch::Uniform);
            preview.Children().Append(placeholder);
            preview.Children().Append(image);
            Border frame;
            frame.Height(136);
            frame.Padding(Thickness{ 2 });
            frame.Child(preview);
            TextBlock label;
            label.Text(std::to_wstring(page + 1));
            label.FontSize(11);
            label.TextAlignment(TextAlignment::Center);
            content.Children().Append(frame);
            content.Children().Append(label);
            ListViewItem item;
            item.Tag(box_value(page));
            item.Content(content);
            PdfThumbnailList().Items().Append(item);
            pdf_thumbnail_images_.push_back(make_weak(image));
        }
        pdf_thumbnail_items_built_ = end;
        if (end < pdf_page_count_)
        {
            const auto weak = get_weak();
            static_cast<void>(DispatcherQueue().TryEnqueue([weak, generation] {
                if (const auto self = weak.get())
                {
                    self->append_pdf_thumbnail_batch(generation);
                }
            }));
            return;
        }
        sync_pdf_thumbnail_selection();
        if (office_emf_preview_)
        {
            continue_office_thumbnail_generation(generation);
        }
        else
        {
            load_pdf_thumbnails_async(generation);
        }
    }

    void MainWindow::navigate_to_pdf_page(std::uint32_t page_index)
    {
        if (pdf_render_client_ == nullptr || page_index >= pdf_page_count_ ||
            page_index == pdf_page_index_)
        {
            return;
        }
        pdf_page_index_ = page_index;
        static_cast<void>(PdfScroller().ChangeView(nullptr, nullptr, 1.0F, true));
        if (office_emf_preview_)
        {
            render_office_page_async(page_index, content_generation_);
        }
        else
        {
            render_pdf_page_async(page_index, content_generation_);
        }
    }

    void MainWindow::show_password_prompt(
        PasswordPromptTarget target,
        bool invalid_password)
    {
        password_prompt_target_ = target;
        PasswordPromptTitle().Text(glance::app::localize(L"PasswordPromptTitle"));
        PasswordPromptError().Text(
            invalid_password ? glance::app::localize(L"PasswordIncorrect") : L"");
        PasswordPromptError().Visibility(
            invalid_password ? Visibility::Visible : Visibility::Collapsed);
        PasswordPromptInput().Password(L"");
        set_password_prompt_activation(true);
        PasswordPromptOverlay().Visibility(Visibility::Visible);
        if (GetForegroundWindow() == window_)
        {
            PasswordPromptInput().Focus(FocusState::Programmatic);
        }
    }

    void MainWindow::hide_password_prompt()
    {
        password_prompt_target_ = PasswordPromptTarget::none;
        PasswordPromptInput().Password(L"");
        PasswordPromptError().Text(L"");
        PasswordPromptError().Visibility(Visibility::Collapsed);
        PasswordPromptOverlay().Visibility(Visibility::Collapsed);
        set_password_prompt_activation(false);
        if (GetForegroundWindow() == window_ && IsWindow(source_window_))
        {
            SetForegroundWindow(source_window_);
        }
    }

    void MainWindow::set_password_prompt_activation(bool enabled) noexcept
    {
        if (window_ == nullptr || password_prompt_activation_enabled_ == enabled)
        {
            return;
        }
        auto extended_style = GetWindowLongPtrW(window_, GWL_EXSTYLE);
        if (enabled)
        {
            extended_style &= ~static_cast<LONG_PTR>(WS_EX_NOACTIVATE);
        }
        else
        {
            extended_style |= WS_EX_NOACTIVATE;
        }
        SetWindowLongPtrW(window_, GWL_EXSTYLE, extended_style);
        SetWindowPos(
            window_,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                SWP_NOACTIVATE | SWP_FRAMECHANGED);
        password_prompt_activation_enabled_ = enabled;
    }

    void MainWindow::submit_password()
    {
        if (PasswordPromptOverlay().Visibility() != Visibility::Visible)
        {
            return;
        }
        const auto target = password_prompt_target_;
        const std::wstring password = PasswordPromptInput().Password().c_str();
        PasswordPromptSubmitButton().IsEnabled(false);
        PasswordPromptError().Visibility(Visibility::Collapsed);
        hide_password_prompt();
        PasswordPromptSubmitButton().IsEnabled(true);
        if (target == PasswordPromptTarget::pdf && !pdf_source_path_.empty())
        {
            PdfLoadingText().Text(glance::app::localize(L"LoadingPdf"));
            PdfLoadingOverlay().Visibility(Visibility::Visible);
            load_pdf_async(pdf_source_path_, content_generation_, password);
        }
        else if (target == PasswordPromptTarget::archive && !archive_source_path_.empty())
        {
            ArchiveStatusText().Text(glance::app::localize(L"LoadingArchive"));
            load_archive_async(archive_source_path_, content_generation_, password);
        }
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
        if (PdfScroller().ZoomFactor() > 1.001F || pdf_render_client_ == nullptr)
        {
            pdf_wheel_delta_ = 0;
            return;
        }

        pdf_wheel_delta_ += args.GetCurrentPoint(PdfFitSurface()).Properties().MouseWheelDelta();
        if (std::abs(pdf_wheel_delta_) >= WHEEL_DELTA)
        {
            if (pdf_wheel_delta_ > 0 && pdf_page_index_ > 0)
            {
                navigate_to_pdf_page(pdf_page_index_ - 1);
            }
            else if (pdf_wheel_delta_ < 0 && pdf_page_index_ + 1 < pdf_page_count_)
            {
                navigate_to_pdf_page(pdf_page_index_ + 1);
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

    void MainWindow::PreviewAsTextButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (basic_info_mode_ ||
            !generic_text_preview_allowed_ ||
            current_kind_ != glance::app::PreviewKind::generic ||
            current_index_ >= files_.size())
        {
            return;
        }

        const auto& file = files_[current_index_];
        if (file.path.empty() || file.is_cloud_placeholder ||
            (file.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
            file.size > maximum_preview_as_text_bytes ||
            !glance::app::can_try_preview_as_text(file.path))
        {
            return;
        }

        ++content_generation_;
        dismiss_preview_info_bar();
        PreviewAsTextButton().IsEnabled(false);
        ErrorText().Visibility(Visibility::Collapsed);
        load_text_async(
            file.path,
            false,
            content_generation_,
            glance::app::TextEncoding::automatic,
            true);
    }

    void MainWindow::GenericAdvancedInfoButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (current_kind_ != glance::app::PreviewKind::generic ||
            current_index_ >= files_.size())
        {
            return;
        }

        generic_preview_preferences_.show_advanced_info =
            GenericAdvancedInfoButton().IsChecked().Value();
        glance::app::save_generic_preview_preferences(generic_preview_preferences_);
        if (!generic_preview_preferences_.show_advanced_info)
        {
            GenericAdvancedInfoText().Text(L"");
            GenericAdvancedInfoScroller().Visibility(Visibility::Collapsed);
            return;
        }

        const auto& file = files_[current_index_];
        if (!file.path.empty() && !file.is_cloud_placeholder &&
            (file.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            load_generic_file_info_async(file.path, content_generation_);
        }
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

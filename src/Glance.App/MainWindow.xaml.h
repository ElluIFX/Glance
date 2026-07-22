#pragma once

#include "MainWindow.g.h"
#include "appearance_preferences.h"
#include "archive_provider.h"
#include "preview_file.h"
#include "preview_provider.h"
#include "path_copy_preferences.h"
#include "text_preferences.h"

#include "glance/contracts/preview_state.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace winrt::Glance::App::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        using StateCallback = std::function<void(
            std::uint64_t,
            glance::contracts::PreviewWindowState)>;

        MainWindow();

        void InitializeSession(std::uint64_t instance_id, StateCallback callback);
        void ShowPreview(
            std::vector<glance::app::PreviewFile> files,
            std::uint32_t focused_index,
            std::uint32_t source_kind,
            HWND source_window);
        void HidePreview();
        void ApplyAppearancePreferences();
        void ApplyLocalizedResources();
        void ApplyTextPreferences();
        [[nodiscard]] std::uint64_t InstanceId() const noexcept { return instance_id_; }

        void TopmostButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ClosePreviewButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void PinButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void MarkdownPreviewButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void MarkdownCodeButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void LineNumbersButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void SyntaxHighlightButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void WordWrapButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void EncodingOption_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void TextPreviewScroller_SizeChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        void TextPreviewScroller_PointerWheelChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void ImagePanel_SizeChanged(IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        void PdfPanel_SizeChanged(IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        void PdfScroller_PointerWheelChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void ImageExifButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ImageScroller_PointerWheelChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void ImageScroller_PointerPressed(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void ImageScroller_PointerMoved(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void ImageScroller_PointerReleased(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void ImageScroller_PointerCanceled(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void ImageScroller_PointerCaptureLost(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void ZoomOutButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ZoomInButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RotateButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void MediaPanel_PointerMoved(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void MediaPlayPauseButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void MediaMuteButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void MediaSeekSlider_ValueChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&);
        void MediaVolumeSlider_ValueChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&);
        void PreviousPdfPageButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void NextPdfPageButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void FileList_SelectionChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void CopyPathButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OpenFolderButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OpenDefaultButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OpenDefaultButton_RightTapped(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::RightTappedRoutedEventArgs const&);
        void LoadCloudFileButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        static LRESULT CALLBACK window_subclass(
            HWND window,
            UINT message,
            WPARAM wparam,
            LPARAM lparam,
            UINT_PTR subclass_id,
            DWORD_PTR reference_data) noexcept;
        void configure_window();
        void position_initial_window(bool ignore_saved_size = false);
        void auto_fit_window_to_content(double width, double height) noexcept;
        [[nodiscard]] bool auto_fit_applies() const noexcept;
        void save_current_window_size() const noexcept;
        void clear_preview_content();
        void reset_hidden_window_size() noexcept;
        void present_file(std::uint32_t index);
        void present_generic(const glance::app::PreviewFile& file);
        winrt::fire_and_forget load_generic_file_info_async(std::wstring path, std::uint64_t generation);
        void present_text(const glance::app::PreviewFile& file, bool markdown);
        winrt::fire_and_forget load_text_async(
            std::wstring path,
            bool markdown,
            std::uint64_t generation,
            glance::app::TextEncoding encoding);
        winrt::fire_and_forget load_image_async(std::wstring path, std::uint64_t generation);
        winrt::fire_and_forget load_image_metadata_async(std::wstring path, std::uint64_t generation);
        winrt::fire_and_forget load_media_async(std::wstring path, std::uint64_t generation);
        winrt::fire_and_forget load_media_technical_metadata_async(
            std::wstring path,
            std::uint64_t generation,
            bool audio,
            std::uint64_t fallback_bitrate);
        winrt::fire_and_forget load_pdf_async(std::wstring path, std::uint64_t generation);
        winrt::fire_and_forget render_pdf_page_async(std::uint32_t page_index, std::uint64_t generation);
        winrt::fire_and_forget load_archive_async(std::wstring path, std::uint64_t generation);
        winrt::fire_and_forget load_directory_async(std::wstring path, std::uint64_t generation);
        winrt::fire_and_forget load_office_async(
            std::wstring path,
            std::uint64_t generation,
            std::uint64_t source_size,
            std::uint64_t source_modified_time);
        void apply_archive_preview(glance::app::ArchivePreview preview, std::uint64_t generation);
        void apply_text_preview(glance::app::TextPreview preview, bool markdown, std::uint64_t generation);
        void render_markdown();
        winrt::fire_and_forget render_markdown_async(std::wstring html, std::uint64_t generation);
        void render_text_content();
        void apply_text_preferences();
        void apply_text_font_metrics();
        void update_text_layout();
        void update_line_numbers();
        void show_text_font_size_overlay();
        void set_markdown_preview_mode(bool preview);
        void update_line_number_visibility();
        void show_content_panel(glance::app::PreviewKind kind);
        void show_provider_error(std::wstring message, std::uint64_t generation);
        void update_image_fit_surface();
        void fit_image_to_viewport();
        void update_pdf_fit_surface();
        void stop_media_playback();
        void show_media_controls();
        void update_media_controls();
        void update_media_footer();
        void update_image_metadata_visibility();
        void set_image_zoom(float zoom, Windows::Foundation::Point anchor);
        void end_image_pan(Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void update_state();
        void set_topmost(bool enabled);
        void start_detached_focus_monitor();
        void stop_detached_focus_monitor();
        [[nodiscard]] std::wstring formatted_size(std::uint64_t size) const;
        [[nodiscard]] std::wstring formatted_time(std::uint64_t file_time) const;

        HWND window_{};
        std::uint64_t instance_id_{};
        StateCallback state_callback_;
        std::vector<glance::app::PreviewFile> files_;
        std::uint32_t current_index_{};
        std::uint32_t source_kind_{};
        HWND source_window_{};
        HWND foreground_when_unpinned_{};
        bool visible_{};
        bool topmost_{};
        bool pinned_{};
        bool detached_{};
        bool user_sized_{};
        bool line_numbers_visible_{ true };
        bool syntax_highlighting_{ true };
        bool word_wrap_{ true };
        bool media_is_audio_{};
        bool updating_media_position_{};
        std::uint32_t media_controls_idle_ticks_{};
        bool markdown_preview_{};
        bool image_metadata_visible_{};
        bool image_panning_{};
        double image_rotation_{};
        std::uint32_t image_pixel_width_{};
        std::uint32_t image_pixel_height_{};
        double image_pan_horizontal_offset_{};
        double image_pan_vertical_offset_{};
        Windows::Foundation::Point image_pan_start_{};
        std::uint64_t content_generation_{};
        std::wstring current_text_;
        std::wstring current_text_path_;
        bool current_text_markdown_{};
        glance::app::TextEncoding current_text_encoding_{ glance::app::TextEncoding::automatic };
        glance::app::TextPreferences text_preferences_{};
        std::wstring image_metadata_;
        std::wstring media_dimensions_;
        std::wstring media_technical_info_;
        std::wstring office_temp_pdf_;
        std::wstring office_cache_source_path_;
        std::uint64_t office_cache_source_size_{};
        std::uint64_t office_cache_source_modified_time_{};
        Windows::Data::Pdf::PdfDocument pdf_document_{ nullptr };
        std::uint32_t pdf_page_index_{};
        int pdf_wheel_delta_{};
        int text_font_wheel_delta_{};
        Microsoft::UI::Xaml::DispatcherTimer focus_timer_{ nullptr };
        Microsoft::UI::Xaml::DispatcherTimer media_timer_{ nullptr };
        Microsoft::UI::Xaml::DispatcherTimer copy_feedback_timer_{ nullptr };
        Microsoft::UI::Xaml::DispatcherTimer font_size_overlay_timer_{ nullptr };
        glance::app::PreviewKind current_kind_{ glance::app::PreviewKind::generic };
        glance::contracts::PreviewWindowState state_{ glance::contracts::PreviewWindowState::hidden };
    };
}

namespace winrt::Glance::App::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}

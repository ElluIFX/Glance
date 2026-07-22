#pragma once

#include "MainWindow.g.h"
#include "appearance_preferences.h"
#include "archive_provider.h"
#include "footer_preferences.h"
#include "folder_preview_preferences.h"
#include "generic_preview_preferences.h"
#include "media_preview_preferences.h"
#include "office_preview_client.h"
#include "preview_file.h"
#include "preview_provider.h"
#include "path_copy_preferences.h"
#include "pdf_render_client.h"
#include "shell_icon_provider.h"
#include "syntax_highlighter.h"
#include "text_preferences.h"

#include "glance/contracts/preview_state.h"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
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
        void ApplyWindowPreferences();
        void ApplyLocalizedResources();
        void ApplyTextPreferences();
        void ApplyFooterPreferences();
        [[nodiscard]] std::uint64_t InstanceId() const noexcept { return instance_id_; }

        void TopmostButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void PreviewModeButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ClosePreviewButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void PinButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void MarkdownPreviewButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void MarkdownCodeButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void LineNumbersButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void SyntaxHighlightButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void WordWrapButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ArchiveHeaderButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void EncodingOption_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void TextPreviewScroller_SizeChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        void TextPreviewScroller_ViewChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::Controls::ScrollViewerViewChangedEventArgs const&);
        void LongTextList_ContainerContentChanging(
            Microsoft::UI::Xaml::Controls::ListViewBase const&,
            Microsoft::UI::Xaml::Controls::ContainerContentChangingEventArgs const&);
        void LongTextList_SizeChanged(
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
        void MediaPanel_PointerWheelChanged(
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
        void PdfThumbnailsButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void PdfOutlineButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void PdfThumbnailList_SelectionChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void PdfOutlineEntry_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void PasswordPromptSubmitButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void PasswordPromptInput_KeyDown(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);
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
        void PreviewAsTextButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void GenericAdvancedInfoButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

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
        [[nodiscard]] bool should_defer_auto_fit_show() const noexcept;
        void reveal_deferred_preview() noexcept;
        void auto_fit_window_to_content(
            double width,
            double height,
            bool dynamic_update = false) noexcept;
        [[nodiscard]] bool auto_fit_applies(bool dynamic_update = false) const noexcept;
        void save_current_window_placement() const noexcept;
        void clear_preview_content();
        void cancel_office_conversion() noexcept;
        void cancel_pdf_render() noexcept;
        void reset_hidden_window_size() noexcept;
        void present_file(std::uint32_t index);
        void present_generic(
            const glance::app::PreviewFile& file,
            bool allow_text_preview = false,
            bool allow_advanced_info = false);
        winrt::fire_and_forget load_generic_icon_async(
            std::wstring path,
            bool is_folder,
            bool use_file_attributes,
            std::uint64_t generation);
        winrt::fire_and_forget load_generic_file_info_async(std::wstring path, std::uint64_t generation);
        winrt::fire_and_forget load_footer_access_async(std::wstring path, std::uint64_t generation);
        void prepare_text_preview(const glance::app::PreviewFile& file, bool markdown);
        void present_text(const glance::app::PreviewFile& file, bool markdown);
        winrt::fire_and_forget load_text_async(
            std::wstring path,
            bool markdown,
            std::uint64_t generation,
            glance::app::TextEncoding encoding,
            bool preview_as_text_attempt = false);
        winrt::fire_and_forget load_next_text_chunk_async(std::uint64_t generation);
        winrt::fire_and_forget load_image_async(std::wstring path, std::uint64_t generation);
        winrt::fire_and_forget load_image_metadata_async(std::wstring path, std::uint64_t generation);
        winrt::fire_and_forget load_media_async(std::wstring path, std::uint64_t generation);
        winrt::fire_and_forget load_media_technical_metadata_async(
            std::wstring path,
            std::uint64_t generation,
            bool audio,
            std::uint64_t fallback_bitrate);
        winrt::fire_and_forget load_pdf_async(
            std::wstring path,
            std::uint64_t generation,
            std::wstring password = {});
        winrt::fire_and_forget render_pdf_page_async(
            std::uint32_t page_index,
            std::uint64_t generation,
            bool dynamic_update = false);
        winrt::fire_and_forget load_pdf_thumbnails_async(std::uint64_t generation);
        void apply_pdf_open_result(
            std::shared_ptr<glance::app::PdfRenderClient> session,
            glance::app::PdfOpenResult result,
            std::wstring path,
            std::wstring password,
            std::uint64_t generation);
        void build_pdf_navigation(std::uint64_t generation);
        void append_pdf_thumbnail_batch(std::uint64_t generation);
        void show_pdf_navigation(bool thumbnails);
        void sync_pdf_thumbnail_selection();
        void navigate_to_pdf_page(std::uint32_t page_index);
        enum class PasswordPromptTarget
        {
            none,
            pdf,
            archive,
        };
        void show_password_prompt(PasswordPromptTarget target, bool invalid_password);
        void hide_password_prompt();
        void set_password_prompt_activation(bool enabled) noexcept;
        void submit_password();
        winrt::fire_and_forget load_archive_async(
            std::wstring path,
            std::uint64_t generation,
            std::wstring password = {});
        winrt::fire_and_forget load_directory_async(std::wstring path, std::uint64_t generation);
        winrt::fire_and_forget load_office_async(
            std::wstring path,
            std::uint64_t generation,
            std::uint64_t source_size,
            std::uint64_t source_modified_time);
        winrt::fire_and_forget load_word_emf_async(
            std::wstring path,
            std::uint64_t generation);
        winrt::fire_and_forget render_office_page_async(
            std::uint32_t page_index,
            std::uint64_t generation,
            bool dynamic_update = false);
        winrt::fire_and_forget load_office_thumbnail_async(
            std::uint32_t page_index,
            std::uint64_t generation,
            bool continue_background = false);
        void continue_office_thumbnail_generation(std::uint64_t generation);
        struct ArchiveIconTarget
        {
            std::wstring path;
            std::wstring cache_key;
            bool is_folder{};
            winrt::weak_ref<Microsoft::UI::Xaml::Controls::Image> image;
            winrt::weak_ref<Microsoft::UI::Xaml::Controls::FontIcon> fallback;
        };
        struct PendingArchiveNode
        {
            const glance::app::ArchiveEntry* entry{};
            Microsoft::UI::Xaml::Controls::TreeViewNode parent{ nullptr };
        };
        struct ArchiveRenderState
        {
            glance::app::ArchivePreview preview;
            std::deque<PendingArchiveNode> pending;
            std::vector<ArchiveIconTarget> icon_targets;
            std::wstring status;
            std::uint64_t generation{};
        };
        void apply_archive_preview(glance::app::ArchivePreview preview, std::uint64_t generation);
        void render_archive_batch(const std::shared_ptr<ArchiveRenderState>& state);
        void update_archive_header_state();
        winrt::fire_and_forget load_archive_icons_async(
            std::vector<ArchiveIconTarget> targets,
            std::uint64_t generation);
        void apply_text_preview(
            glance::app::TextPreview preview,
            bool markdown,
            std::uint64_t generation,
            bool preview_as_text_attempt);
        void render_markdown();
        winrt::fire_and_forget render_markdown_async(std::wstring html, std::uint64_t generation);
        void render_text_content();
        void append_syntax_ranges(std::wstring_view content);
        void append_text_content(std::wstring_view content);
        void rebuild_long_text_content();
        void append_long_text_content(std::wstring_view content);
        void set_line_number_text(std::wstring text);
        void append_line_numbers(std::wstring_view content);
        void update_virtual_line_numbers();
        void ensure_text_viewport_filled();
        void queue_visible_syntax_highlight_update();
        void update_visible_syntax_highlights();
        void apply_text_preferences();
        void apply_text_font_metrics();
        void update_text_layout();
        void update_line_numbers();
        void show_text_font_size_overlay();
        void set_markdown_preview_mode(bool preview);
        void update_line_number_visibility();
        void show_content_panel(glance::app::PreviewKind kind);
        void dismiss_preview_info_bar();
        void show_preview_notice(std::wstring resource_key);
        void animate_preview_info_bar(bool opening);
        void show_syntax_highlight_disabled_notice();
        void show_text_preview_error(std::wstring message);
        void show_provider_error(std::wstring message, std::uint64_t generation);
        void update_image_fit_surface();
        void fit_image_to_viewport();
        void update_pdf_fit_surface();
        void stop_media_playback();
        void show_media_controls();
        void update_media_controls();
        void update_media_footer();
        void update_footer_metadata();
        void update_generic_file_metadata();
        void request_footer_access_if_needed();
        void update_preview_mode_button();
        void update_preview_as_text_button();
        void update_image_metadata_visibility();
        void set_image_zoom(float zoom, Windows::Foundation::Point anchor);
        void end_image_pan(Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void update_state();
        void update_window_action_visibility();
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
        bool defer_auto_fit_show_{};
        bool user_sized_{};
        bool tracking_move_size_{};
        RECT move_size_start_bounds_{};
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
        std::wstring line_number_text_;
        std::wstring current_text_path_;
        bool current_text_markdown_{};
        std::shared_ptr<glance::app::IncrementalTextReader> current_text_reader_;
        glance::app::SyntaxHighlightState syntax_highlight_state_{};
        struct TextSyntaxRange
        {
            std::size_t start{};
            std::size_t length{};
            glance::app::SyntaxStyle style{};
        };
        std::vector<TextSyntaxRange> text_syntax_ranges_;
        struct TextParagraphRange
        {
            std::size_t start{};
            std::size_t length{};
        };
        std::vector<TextParagraphRange> text_paragraph_ranges_;
        std::wstring text_render_tail_;
        bool text_tail_block_attached_{};
        std::wstring long_text_render_tail_;
        std::vector<TextParagraphRange> long_text_block_ranges_;
        std::vector<std::size_t> long_text_block_start_lines_;
        std::size_t long_text_next_line_{ 1 };
        std::size_t text_highlight_offset_{};
        std::size_t styled_paragraph_start_{ std::numeric_limits<std::size_t>::max() };
        std::size_t styled_paragraph_end_{ std::numeric_limits<std::size_t>::max() };
        bool syntax_highlight_layout_dirty_{};
        double syntax_highlight_vertical_offset_{ -1.0 };
        std::size_t text_line_count_{};
        bool current_text_has_more_{};
        bool text_chunk_loading_{};
        bool line_numbers_simple_{};
        bool virtual_line_numbers_{};
        bool long_text_mode_{};
        bool syntax_highlight_update_queued_{};
        bool updating_syntax_highlights_{};
        glance::app::TextEncoding current_text_encoding_{ glance::app::TextEncoding::automatic };
        glance::app::TextPreferences text_preferences_{};
        glance::app::FooterPreferences footer_preferences_{};
        glance::app::FolderPreviewPreferences folder_preview_preferences_{};
        glance::app::GenericPreviewPreferences generic_preview_preferences_{};
        std::shared_ptr<ArchiveRenderState> archive_render_state_;
        bool archive_preview_is_directory_{};
        bool archive_entry_compressed_size_available_{};
        std::wstring archive_source_path_;
        std::wstring archive_password_;
        PasswordPromptTarget password_prompt_target_{ PasswordPromptTarget::none };
        bool password_prompt_activation_enabled_{};
        std::wstring footer_access_mode_;
        bool footer_access_loaded_{};
        bool footer_access_requested_{};
        std::wstring image_metadata_;
        std::wstring media_dimensions_;
        std::wstring media_technical_info_;
        struct OfficeConversionOperation
        {
            void attach_process(HANDLE value) noexcept;
            void detach_process(HANDLE value) noexcept;
            void cancel() noexcept;
            [[nodiscard]] bool is_cancelled() const noexcept;

            std::atomic_bool cancelled{};
            std::mutex process_mutex;
            HANDLE process{};
        };
        std::shared_ptr<OfficeConversionOperation> office_conversion_;
        std::shared_ptr<glance::app::OfficePreviewClient> office_preview_client_;
        bool office_emf_preview_{};
        bool office_thumbnail_background_active_{};
        std::vector<bool> office_thumbnail_requested_;
        std::wstring office_temp_pdf_;
        std::wstring office_cache_source_path_;
        std::uint64_t office_cache_source_size_{};
        std::uint64_t office_cache_source_modified_time_{};
        std::shared_ptr<glance::app::PdfRenderClient> pdf_render_client_;
        std::wstring pdf_source_path_;
        std::wstring pdf_password_;
        std::vector<glance::app::PdfOutlineEntry> pdf_outline_;
        std::vector<winrt::weak_ref<Microsoft::UI::Xaml::Controls::Image>> pdf_thumbnail_images_;
        std::uint32_t pdf_page_count_{};
        std::uint32_t pdf_thumbnail_items_built_{};
        bool pdf_thumbnail_selection_updating_{};
        std::uint32_t pdf_page_index_{};
        std::atomic_uint64_t pdf_render_request_{};
        int pdf_wheel_delta_{};
        int text_font_wheel_delta_{};
        int media_seek_wheel_delta_{};
        int media_volume_wheel_delta_{};
        Microsoft::UI::Xaml::DispatcherTimer focus_timer_{ nullptr };
        Microsoft::UI::Xaml::DispatcherTimer media_timer_{ nullptr };
        Microsoft::UI::Xaml::DispatcherTimer copy_feedback_timer_{ nullptr };
        Microsoft::UI::Xaml::DispatcherTimer font_size_overlay_timer_{ nullptr };
        Microsoft::UI::Xaml::DispatcherTimer preview_notice_timer_{ nullptr };
        Microsoft::UI::Xaml::DispatcherTimer preview_notice_hide_timer_{ nullptr };
        bool syntax_highlight_notice_pending_{};
        bool preview_notice_active_{};
        bool preview_notice_hiding_{};
        std::wstring preview_notice_resource_key_;
        glance::app::PreviewKind current_kind_{ glance::app::PreviewKind::generic };
        glance::app::PreviewKind content_preview_kind_{ glance::app::PreviewKind::generic };
        bool basic_info_mode_{};
        bool generic_text_preview_allowed_{};
        glance::contracts::PreviewWindowState state_{ glance::contracts::PreviewWindowState::hidden };
    };
}

namespace winrt::Glance::App::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}

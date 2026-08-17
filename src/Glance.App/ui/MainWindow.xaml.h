#pragma once

#include "MainWindow.g.h"
#include "appearance_preferences.h"
#include "archive_provider.h"
#include "component_loader.h"
#include "footer_preferences.h"
#include "folder_preview_preferences.h"
#include "generic_preview_preferences.h"
#include "media_preview_preferences.h"
#include "preview_file.h"
#include "preview_provider.h"
#include "path_copy_preferences.h"
#include "paged_document_render_client.h"
#include "scintilla_text_view.h"
#include "shell_icon_provider.h"
#include "text_preferences.h"
#include "window_acrylic_backdrop.h"

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
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace winrt::Glance::App::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        using StateCallback = std::function<void(
            std::uint64_t,
            glance::contracts::PreviewWindowState)>;
        using GalleryRequestCallback = std::function<bool(std::string)>;
        using ComponentActionCallback =
            std::function<void(std::wstring, std::wstring)>;

        MainWindow();

        void InitializeSession(
            std::uint64_t instance_id,
            StateCallback callback,
            GalleryRequestCallback gallery_request_callback,
            ComponentActionCallback component_action_callback);
        void ShowPreview(
            std::vector<glance::app::PreviewFile> files,
            std::uint32_t focused_index,
            std::uint32_t source_kind,
            HWND source_window,
            std::wstring source_id = {},
            std::uint64_t source_capabilities = 0);
        [[nodiscard]] bool IsPreviewingFile(const std::wstring& path) const noexcept;
        void CloseForReplacement();
        void HidePreview();
        [[nodiscard]] bool ActivateSelectedFolderEntry();
        [[nodiscard]] bool NavigateBack();
        void ApplyAppearancePreferences();
        void ApplyLocalizedResources();
        void ApplyTextPreferences();
        void ApplyFooterPreferences();
        void ApplyWindowPreferences();
        void RefreshComponentContributions();
        void HandleGalleryResponse(std::string_view payload);
        void HandleGalleryDisconnect();
        [[nodiscard]] std::uint64_t InstanceId() const noexcept { return instance_id_; }

        void TopmostButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void PreviewModeButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void FullscreenButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ClosePreviewButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void BackButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void PinButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void MarkdownPreviewButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void MarkdownCodeButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void LineNumbersButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void SyntaxHighlightButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void WordWrapButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ArchiveHeaderButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void FolderEntryList_DoubleTapped(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);
        void PreviewContentHost_DoubleTapped(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);
        void EncodingOption_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void TextEditorHost_Loaded(
            IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void TextEditorHost_SizeChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        void ImagePanel_SizeChanged(IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        void PdfPanel_SizeChanged(IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        void PdfScroller_PointerWheelChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void PdfScroller_PointerPressed(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void PdfScroller_PointerMoved(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void PdfScroller_PointerReleased(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void PdfScroller_PointerCanceled(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void PdfScroller_PointerCaptureLost(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void ImageExifButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ImageExifButton_RightTapped(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::RightTappedRoutedEventArgs const&);
        void ImageScroller_PointerWheelChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void ImageScroller_ViewChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::Controls::ScrollViewerViewChangedEventArgs const&);
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
        void ImageZoomMapOverlay_PointerPressed(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void ImageZoomMapOverlay_PointerMoved(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void ImageZoomMapOverlay_PointerReleased(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void ImageZoomMapOverlay_PointerCanceled(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void ImageZoomMapOverlay_PointerCaptureLost(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void ImageZoomButton_RightTapped(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::RightTappedRoutedEventArgs const&);
        void ImageZoomSlider_ValueChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&);
        void RotateButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RotateButton_RightTapped(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::RightTappedRoutedEventArgs const&);
        void FlipButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void FlipButton_RightTapped(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::RightTappedRoutedEventArgs const&);
        void MediaPanel_PointerMoved(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void MediaPanel_PointerPressed(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void MediaPanel_PointerWheelChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void MediaPlayPauseButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void MediaMuteButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void GalleryModeButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void GalleryModeButton_RightTapped(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::RightTappedRoutedEventArgs const&);
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
        void PreviewErrorInfoBar_Closed(
            Microsoft::UI::Xaml::Controls::InfoBar const&,
            Microsoft::UI::Xaml::Controls::InfoBarClosedEventArgs const&);
        void FileList_SelectionChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void CopyPathButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget CopyPathButton_RightTapped(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::RightTappedRoutedEventArgs const&);
        void OpenFolderButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OpenDefaultButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OpenDefaultButton_RightTapped(
            IInspectable const&,
            Microsoft::UI::Xaml::Input::RightTappedRoutedEventArgs const&);
        void LoadCloudFileButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void PreviewAsTextButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void GenericAdvancedInfoButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        enum class GalleryMode
        {
            inactive,
            opening,
            active,
        };

        void show_copy_feedback(
            const Microsoft::UI::Xaml::Controls::FontIcon& icon);
        void copy_text_to_clipboard(
            std::wstring_view text,
            std::wstring_view operation,
            const Microsoft::UI::Xaml::Controls::FontIcon& feedback_icon);
        static LRESULT CALLBACK window_subclass(
            HWND window,
            UINT message,
            WPARAM wparam,
            LPARAM lparam,
            UINT_PTR subclass_id,
            DWORD_PTR reference_data) noexcept;
        void configure_window();
        void set_fullscreen(bool enabled) noexcept;
        void update_fullscreen_button();
        void update_fullscreen_chrome() noexcept;
        void update_fullscreen_chrome_surfaces();
        void set_fullscreen_chrome_visibility(bool title_visible, bool footer_visible);
        [[nodiscard]] bool handle_preview_content_double_click();
        [[nodiscard]] bool is_interactive_preview_source(
            IInspectable const& source);
        void position_initial_window(bool ignore_saved_size = false);
        [[nodiscard]] bool should_defer_auto_fit_show(
            glance::app::PreviewKind kind) const noexcept;
        void show_prepared_window() noexcept;
        void reveal_deferred_preview() noexcept;
        void auto_fit_window_to_content(
            double width,
            double height,
            bool dynamic_update = false) noexcept;
        [[nodiscard]] bool auto_fit_applies(bool dynamic_update = false) const noexcept;
        void save_current_window_placement() const noexcept;
        void clear_preview_content();
        void update_preview_navigation_ui();
        [[nodiscard]] const glance::app::ArchiveEntry* selected_folder_entry() noexcept;
        void cancel_pdf_render() noexcept;
        void reset_hidden_window_size() noexcept;
        void present_file(
            std::uint32_t index,
            std::optional<glance::app::PreviewKind> known_kind = std::nullopt);
        void present_generic(
            const glance::app::PreviewFile& file,
            bool allow_text_preview = false,
            bool allow_advanced_info = false);
        winrt::fire_and_forget materialize_shell_file_async(
            std::uint32_t index,
            std::wstring parsing_name,
            std::wstring display_name,
            std::vector<std::uint8_t> shell_id_list,
            std::uint64_t generation,
            std::shared_ptr<std::atomic_bool> cancellation);
        winrt::fire_and_forget load_generic_icon_async(
            std::wstring path,
            bool is_folder,
            bool use_file_attributes,
            std::uint64_t generation);
        winrt::fire_and_forget load_generic_file_info_async(std::wstring path, std::uint64_t generation);
        winrt::fire_and_forget load_footer_access_async(std::wstring path, std::uint64_t generation);
        bool prepare_text_preview(const glance::app::PreviewFile& file, bool markdown, bool web = false);
        void present_text(const glance::app::PreviewFile& file, bool markdown, bool web = false);
        winrt::fire_and_forget load_text_async(
            std::wstring path,
            bool markdown,
            bool web,
            std::uint64_t generation,
            glance::app::TextEncoding encoding,
            bool preview_as_text_attempt = false);
        winrt::fire_and_forget load_next_text_chunk_async(std::uint64_t generation);
        void set_text_loading(bool loading);
        Windows::Foundation::IAsyncAction load_image_async(
            std::wstring path,
            std::uint64_t generation,
            bool first_frame_presented = false);
        Windows::Foundation::IAsyncAction preload_gallery_image_async(
            glance::app::PreviewFile file,
            std::uint64_t generation);
        winrt::fire_and_forget load_image_metadata_async(
            std::wstring path,
            std::uint64_t generation,
            std::shared_ptr<void> component_preview);
        winrt::fire_and_forget load_image_media_info_async(
            std::wstring path,
            std::uint64_t generation);
        winrt::fire_and_forget load_media_async(std::wstring path, std::uint64_t generation);
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
            std::shared_ptr<glance::app::PagedDocumentRenderClient> session,
            glance::app::PagedDocumentOpenResult result,
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
        void cancel_archive_icon_load() noexcept;
        winrt::fire_and_forget load_directory_async(std::wstring path, std::uint64_t generation);
        winrt::fire_and_forget load_component_async(
            std::wstring path,
            std::uint64_t generation);
        winrt::fire_and_forget load_component_file_directory_async(
            std::shared_ptr<void> session,
            std::wstring password,
            std::uint64_t generation);
        winrt::fire_and_forget refresh_component_loading_text_async(
            std::wstring path,
            std::uint64_t generation);
        void present_resolved_file(
            const glance::app::PreviewFile& file,
            glance::app::PreviewKind kind,
            std::uint64_t generation);
        void apply_component_preview(
            glance::app::ComponentPreviewResult result,
            std::uint64_t generation);
        void begin_component_refinement(std::uint64_t generation);
        winrt::fire_and_forget refine_component_preview_async(
            std::shared_ptr<void> refinement,
            std::wstring notice,
            std::uint64_t generation);
        winrt::fire_and_forget apply_component_refinement_async(
            glance::app::ComponentPreviewResult result,
            std::shared_ptr<void> refinement,
            std::uint64_t generation);
        struct ArchiveIconTarget
        {
            std::wstring path;
            std::wstring cache_key;
            std::size_t control_index{};
            std::uint32_t pixel_size{};
            bool is_folder{};
            bool thumbnail_candidate{};
        };
        struct ArchiveIconControl
        {
            Microsoft::UI::Xaml::Controls::Image image{ nullptr };
            Microsoft::UI::Xaml::Controls::FontIcon fallback{ nullptr };
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
            std::vector<ArchiveIconControl> icon_controls;
            std::wstring status;
            std::uint64_t generation{};
        };
        struct GalleryImageCacheEntry
        {
            glance::app::PreviewFile file;
            Microsoft::UI::Xaml::Media::Imaging::BitmapImage bitmap{ nullptr };
            std::uint32_t pixel_width{};
            std::uint32_t pixel_height{};
            std::uint64_t decoded_bytes{};
        };
        struct PreviewNavigationEntry
        {
            glance::app::PreviewFile file;
            std::wstring selected_path;
            RECT window_bounds{};
            bool window_bounds_valid{};
            double folder_scroll_offset{};
            bool folder_scroll_offset_valid{};
        };
        void apply_archive_preview(glance::app::ArchivePreview preview, std::uint64_t generation);
        void render_archive_batch(const std::shared_ptr<ArchiveRenderState>& state);
        void update_archive_header_state();
        winrt::fire_and_forget load_archive_icons_async(
            std::vector<ArchiveIconTarget> targets,
            std::uint64_t generation,
            std::shared_ptr<std::atomic_bool> cancellation);
        void apply_text_preview(
            glance::app::TextPreview preview,
            bool markdown,
            bool web,
            std::uint64_t generation,
            bool preview_as_text_attempt);
        winrt::fire_and_forget render_markdown();
        winrt::fire_and_forget initialize_markdown_web_view_async(std::uint64_t generation);
        winrt::fire_and_forget render_markdown_async(std::wstring html, std::uint64_t generation);
        winrt::fire_and_forget render_web_document_async(std::wstring path, std::uint64_t generation);
        Microsoft::UI::Xaml::Controls::WebView2 ensure_web_view_control();
        winrt::Windows::Foundation::IAsyncAction configure_web_view_core(
            Microsoft::Web::WebView2::Core::CoreWebView2 const& core);
        void clear_web_resource_mappings(
            Microsoft::Web::WebView2::Core::CoreWebView2 const& core) noexcept;
        void clear_web_view_content() noexcept;
        void update_web_view_idle_state();
        void release_web_view_control() noexcept;
        void release_large_preview_buffers();
        bool ensure_text_editor();
        void update_text_editor_bounds() noexcept;
        void update_text_editor_occlusions() noexcept;
        void queue_text_editor_occlusion_update();
        void update_text_editor_visibility() noexcept;
        void ensure_text_viewport_filled();
        void apply_text_preferences();
        void apply_background_surfaces(bool acrylic_enabled);
        void update_media_surface_background();
        void apply_text_font_metrics();
        void update_text_layout();
        void adjust_text_font_size(int steps);
        void show_text_font_size_overlay();
        void update_title_text();
        void toggle_gallery_mode();
        void open_gallery(bool preserve_navigation = false);
        void leave_gallery(bool show_notice, bool notify_core = true);
        void navigate_gallery(int steps);
        [[nodiscard]] bool handle_gallery_wheel(int delta);
        [[nodiscard]] bool gallery_source_available() const noexcept;
        void request_gallery_page(std::uint32_t target_index, bool select_after_load = true);
        void request_gallery_selection(std::uint32_t target_index);
        [[nodiscard]] bool send_gallery_request(
            std::wstring_view operation,
            std::uint64_t request_id,
            std::uint32_t page_start = 0,
            std::uint32_t target_index = 0,
            int navigation_steps = 0);
        void apply_gallery_file(std::uint32_t index, glance::app::PreviewFile file);
        void schedule_gallery_preloads();
        void cancel_gallery_preloads() noexcept;
        [[nodiscard]] std::wstring gallery_image_cache_key(
            const glance::app::PreviewFile& file) const;
        void set_markdown_preview_mode(bool preview);
        void update_line_number_visibility();
        void show_content_panel(glance::app::PreviewKind kind);
        void rebuild_component_contributions();
        void reset_component_hover_info() noexcept;
        void activate_component_shortcut(
            const glance::app::ComponentStatusBarShortcut& shortcut,
            const Microsoft::UI::Xaml::Controls::Primitives::ToggleButton& button);
        winrt::fire_and_forget load_component_hover_info_async(
            glance::app::ComponentStatusBarActivation activation,
            std::wstring path,
            std::uint64_t generation,
            std::shared_ptr<std::atomic_bool> cancellation);
        winrt::fire_and_forget copy_component_shortcut_data_async(
            glance::app::ComponentStatusBarShortcut shortcut,
            Microsoft::UI::Xaml::Controls::FontIcon feedback_icon);
        winrt::fire_and_forget confirm_component_action(
            glance::app::ComponentManagementAction action);
        void dismiss_preview_info_bar();
        void show_preview_notice(std::wstring resource_key);
        void show_preview_message(
            std::wstring message,
            Microsoft::UI::Xaml::Controls::InfoBarSeverity severity,
            bool auto_hide);
        void animate_preview_info_bar(bool opening);
        void show_text_preview_error(std::wstring message);
        void show_provider_error(std::wstring message, std::uint64_t generation);
        void update_image_fit_surface();
        void fit_image_to_viewport();
        void update_image_zoom_map();
        void move_image_viewport_from_zoom_map(Windows::Foundation::Point position);
        void end_image_zoom_map_pan(
            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void rotate_image(double degrees);
        void flip_image(bool horizontal);
        void update_pdf_fit_surface();
        void stop_media_playback();
        void show_media_controls();
        void update_media_controls();
        void update_media_footer();
        void update_media_playback_metadata(
            const Windows::Media::Playback::MediaPlaybackItem& item,
            std::uint64_t generation);
        void update_footer_metadata();
        void update_generic_file_metadata();
        void request_footer_access_if_needed();
        void update_preview_mode_button();
        void update_preview_as_text_button();
        void update_image_metadata_visibility();
        void set_image_zoom(float zoom, Windows::Foundation::Point anchor);
        void update_image_zoom_controls();
        void update_image_transform_controls();
        void end_image_pan(Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void end_pdf_pan(Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
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
        GalleryRequestCallback gallery_request_callback_;
        ComponentActionCallback component_action_callback_;
        std::vector<glance::app::PreviewFile> files_;
        std::uint32_t current_index_{};
        std::uint32_t source_kind_{};
        HWND source_window_{};
        std::wstring source_id_;
        std::uint64_t source_capabilities_{};
        HWND foreground_when_unpinned_{};
        bool visible_{};
        bool fullscreen_{};
        bool fullscreen_title_visible_{ true };
        bool fullscreen_footer_visible_{ true };
        bool double_click_fullscreen_enabled_{};
        bool fullscreen_toggle_pending_{};
        bool topmost_{};
        bool pinned_{};
        bool detached_{};
        bool defer_auto_fit_show_{};
        bool user_sized_{};
        bool tracking_move_size_{};
        RECT move_size_start_bounds_{};
        WINDOWPLACEMENT fullscreen_restore_placement_{ sizeof(WINDOWPLACEMENT) };
        bool fullscreen_restore_placement_valid_{};
        ULONGLONG fullscreen_title_hover_tick_{};
        ULONGLONG fullscreen_footer_hover_tick_{};
        bool line_numbers_visible_{ true };
        bool syntax_highlighting_{ true };
        bool word_wrap_{ true };
        bool media_is_audio_{};
        bool reverse_media_seek_wheel_{};
        bool updating_media_position_{};
        std::uint32_t media_controls_idle_ticks_{};
        bool markdown_preview_{};
        bool web_preview_available_{};
        bool web_view_initializing_{};
        bool web_view_ready_{};
        bool web_view_handlers_registered_{};
        bool web_content_ready_{};
        std::uint64_t web_navigation_generation_{};
        std::uint64_t web_navigation_id_{};
        bool image_metadata_visible_{};
        bool image_panning_{};
        bool image_zoom_map_panning_{};
        bool image_zoom_map_enabled_{ true };
        GalleryMode gallery_mode_{ GalleryMode::inactive };
        glance::contracts::components::GalleryMediaKind gallery_media_kind_{
            glance::contracts::components::GalleryMediaKind::none };
        bool middle_click_gallery_enabled_{ true };
        bool loop_gallery_enabled_{ true };
        bool gallery_same_extension_only_{};
        bool gallery_same_extension_override_{};
        std::uint64_t gallery_session_id_{};
        std::uint64_t gallery_request_sequence_{};
        std::uint64_t gallery_open_request_id_{};
        std::uint64_t gallery_page_request_id_{};
        std::uint64_t gallery_select_request_id_{};
        std::uint32_t gallery_total_count_{};
        bool gallery_total_known_{ true };
        std::uint32_t gallery_current_index_{};
        std::uint32_t gallery_desired_index_{};
        std::optional<std::uint32_t> gallery_pending_target_;
        bool gallery_page_select_after_load_{};
        int gallery_pending_navigation_steps_{};
        int gallery_wheel_delta_{};
        std::unordered_map<std::uint32_t, glance::app::PreviewFile> gallery_items_;
        std::unordered_map<std::wstring, GalleryImageCacheEntry> gallery_image_cache_;
        std::optional<GalleryImageCacheEntry> pending_gallery_image_;
        std::vector<Windows::Foundation::IAsyncAction> gallery_preload_operations_;
        Windows::Foundation::IAsyncAction image_load_operation_{ nullptr };
        std::uint64_t gallery_preload_generation_{};
        double image_rotation_{};
        double image_scale_x_{ 1.0 };
        double image_scale_y_{ 1.0 };
        std::uint32_t image_pixel_width_{};
        std::uint32_t image_pixel_height_{};
        std::uint32_t image_bits_per_pixel_{};
        double image_pan_horizontal_offset_{};
        double image_pan_vertical_offset_{};
        Windows::Foundation::Point image_pan_start_{};
        std::uint64_t content_generation_{};
        std::shared_ptr<std::atomic_bool> shell_file_cancellation_;
        std::uint64_t component_placement_generation_{};
        std::wstring current_text_;
        std::wstring current_text_path_;
        bool current_text_markdown_{};
        bool current_text_web_{};
        std::shared_ptr<glance::app::IncrementalTextReader> current_text_reader_;
        std::unique_ptr<glance::app::ScintillaTextView> text_editor_;
        std::unique_ptr<glance::app::WindowAcrylicBackdrop> acrylic_backdrop_;
        bool acrylic_enabled_{};
        bool current_text_has_more_{};
        bool text_chunk_loading_{};
        bool text_loading_{};
        glance::app::TextEncoding current_text_encoding_{ glance::app::TextEncoding::automatic };
        glance::app::TextPreferences text_preferences_{};
        glance::app::FooterPreferences footer_preferences_{};
        glance::app::FolderPreviewPreferences folder_preview_preferences_{};
        glance::app::GenericPreviewPreferences generic_preview_preferences_{};
        std::shared_ptr<ArchiveRenderState> archive_render_state_;
        std::shared_ptr<std::atomic_bool> archive_icon_cancellation_;
        std::vector<PreviewNavigationEntry> preview_navigation_;
        std::wstring pending_folder_selection_path_;
        double pending_folder_scroll_offset_{};
        bool pending_folder_scroll_offset_valid_{};
        bool pending_folder_focus_restore_{};
        bool archive_preview_is_directory_{};
        bool archive_entry_compressed_size_available_{};
        PasswordPromptTarget password_prompt_target_{ PasswordPromptTarget::none };
        bool password_prompt_activation_enabled_{};
        bool password_prompt_focused_{};
        std::wstring footer_access_mode_;
        bool footer_access_loaded_{};
        bool footer_access_requested_{};
        std::wstring image_metadata_;
        std::wstring image_metadata_json_;
        std::wstring image_taken_time_;
        std::wstring media_dimensions_;
        std::wstring media_playback_info_;
        Windows::Media::Playback::MediaPlaybackItem media_playback_item_{ nullptr };
        std::uint64_t media_playback_generation_{};
        std::shared_ptr<std::atomic_bool> component_hover_cancellation_;
        std::shared_ptr<std::atomic_bool> component_data_copy_cancellation_;
        glance::app::ComponentStatusBarActivation active_component_hover_;
        std::wstring component_hover_info_text_;
        std::wstring component_hover_cache_component_id_;
        std::wstring component_hover_cache_info_id_;
        std::shared_ptr<glance::app::PagedDocumentRenderClient> pdf_render_client_;
        std::shared_ptr<void> active_component_preview_;
        std::shared_ptr<void> active_component_file_directory_;
        glance::app::FileDirectoryDescriptor active_file_directory_descriptor_;
        std::vector<std::uint32_t> active_file_directory_columns_;
        std::shared_ptr<void> active_component_refinement_;
        std::shared_ptr<glance::app::ComponentWebPreview> active_component_web_preview_;
        std::wstring component_refinement_text_;
        std::wstring component_loading_language_;
        std::wstring pdf_source_path_;
        std::wstring pdf_password_;
        std::vector<glance::app::PagedDocumentOutlineEntry> pdf_outline_;
        std::vector<winrt::weak_ref<Microsoft::UI::Xaml::Controls::Image>> pdf_thumbnail_images_;
        std::uint32_t pdf_page_count_{};
        std::uint32_t pdf_thumbnail_items_built_{};
        bool pdf_thumbnail_selection_updating_{};
        bool component_refinement_started_{};
        bool pdf_panning_{};
        std::uint32_t pdf_page_index_{};
        double pdf_pan_horizontal_offset_{};
        double pdf_pan_vertical_offset_{};
        Windows::Foundation::Point pdf_pan_start_{};
        std::atomic_uint64_t pdf_render_request_{};
        std::atomic_uint32_t pdf_foreground_render_requests_{};
        int pdf_wheel_delta_{};
        int media_seek_wheel_delta_{};
        int media_volume_wheel_delta_{};
        Microsoft::UI::Xaml::DispatcherTimer focus_timer_{ nullptr };
        Microsoft::UI::Xaml::DispatcherTimer fullscreen_chrome_timer_{ nullptr };
        Microsoft::UI::Xaml::DispatcherTimer media_timer_{ nullptr };
        Microsoft::UI::Xaml::DispatcherTimer copy_feedback_timer_{ nullptr };
        Microsoft::UI::Xaml::Controls::FontIcon copy_feedback_icon_{ nullptr };
        winrt::hstring copy_feedback_original_glyph_;
        Microsoft::UI::Xaml::DispatcherTimer font_size_overlay_timer_{ nullptr };
        Microsoft::UI::Xaml::DispatcherTimer preview_notice_timer_{ nullptr };
        Microsoft::UI::Xaml::DispatcherTimer preview_notice_hide_timer_{ nullptr };
        Microsoft::UI::Xaml::DispatcherTimer web_view_idle_timer_{ nullptr };
        Microsoft::UI::Xaml::Controls::WebView2 web_preview_{ nullptr };
        std::vector<std::wstring> web_resource_mapping_hosts_;
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

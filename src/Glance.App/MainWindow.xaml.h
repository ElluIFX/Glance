#pragma once

#include "MainWindow.g.h"
#include "archive_provider.h"
#include "preview_file.h"
#include "preview_provider.h"

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
        [[nodiscard]] std::uint64_t InstanceId() const noexcept { return instance_id_; }

        void TopmostButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void FitContentButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void PinButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void MarkdownModeButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ImagePanel_SizeChanged(IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        void ZoomOutButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ZoomInButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RotateLeftButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RotateRightButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void PreviousPdfPageButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void NextPdfPageButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void FileList_SelectionChanged(
            IInspectable const&,
            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void CopyPathButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OpenFolderButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OpenDefaultButton_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ConfirmCloudLoad_Click(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        static LRESULT CALLBACK window_subclass(
            HWND window,
            UINT message,
            WPARAM wparam,
            LPARAM lparam,
            UINT_PTR subclass_id,
            DWORD_PTR reference_data) noexcept;
        void configure_window();
        void position_initial_window();
        void present_file(std::uint32_t index);
        void present_generic(const glance::app::PreviewFile& file);
        void present_text(const glance::app::PreviewFile& file, bool markdown);
        winrt::fire_and_forget load_text_async(std::wstring path, bool markdown, std::uint64_t generation);
        winrt::fire_and_forget load_image_async(std::wstring path, std::uint64_t generation);
        winrt::fire_and_forget load_media_async(std::wstring path, std::uint64_t generation);
        winrt::fire_and_forget load_pdf_async(std::wstring path, std::uint64_t generation);
        winrt::fire_and_forget render_pdf_page_async(std::uint32_t page_index, std::uint64_t generation);
        winrt::fire_and_forget load_archive_async(std::wstring path, std::uint64_t generation);
        winrt::fire_and_forget load_office_async(std::wstring path, std::uint64_t generation);
        void apply_archive_preview(glance::app::ArchivePreview preview, std::uint64_t generation);
        void apply_text_preview(glance::app::TextPreview preview, bool markdown, std::uint64_t generation);
        void render_markdown();
        void show_content_panel(glance::app::PreviewKind kind);
        void show_provider_error(std::wstring message, std::uint64_t generation);
        void update_image_fit_surface();
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
        bool markdown_preview_{};
        double image_rotation_{};
        std::uint64_t content_generation_{};
        std::wstring current_text_;
        std::wstring office_temp_pdf_;
        Windows::Data::Pdf::PdfDocument pdf_document_{ nullptr };
        std::uint32_t pdf_page_index_{};
        Microsoft::UI::Xaml::DispatcherTimer focus_timer_{ nullptr };
        glance::contracts::PreviewWindowState state_{ glance::contracts::PreviewWindowState::hidden };
    };
}

namespace winrt::Glance::App::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}

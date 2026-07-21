#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <microsoft.ui.xaml.window.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <sstream>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Documents;

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
}

namespace winrt::Glance::App::implementation
{
    MainWindow::MainWindow()
    {
        InitializeComponent();
        configure_window();
    }

    void MainWindow::InitializeSession(std::uint64_t instance_id, StateCallback callback)
    {
        instance_id_ = instance_id;
        state_callback_ = std::move(callback);
    }

    void MainWindow::configure_window()
    {
        const auto window_native = this->try_as<::IWindowNative>();
        check_hresult(window_native->get_WindowHandle(&window_));

        LONG_PTR extended_style = GetWindowLongPtrW(window_, GWL_EXSTYLE);
        extended_style |= WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
        SetWindowLongPtrW(window_, GWL_EXSTYLE, extended_style);
        SetWindowSubclass(window_, window_subclass, 1, reinterpret_cast<DWORD_PTR>(this));

        ExtendsContentIntoTitleBar(true);
        SetTitleBar(TitleBarDragRegion());
        Title(L"Glance");
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
        }
        if (message == WM_SYSCOMMAND && self != nullptr &&
            (wparam & 0xFFF0U) == SC_MAXIMIZE)
        {
            self->user_sized_ = true;
        }
        if (message == WM_NCDESTROY && self != nullptr)
        {
            RemoveWindowSubclass(window, window_subclass, 1);
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
        state_ = glance::contracts::PreviewWindowState::hidden;
        if (state_callback_)
        {
            state_callback_(instance_id_, state_);
        }
    }

    void MainWindow::position_initial_window()
    {
        HMONITOR monitor = MonitorFromWindow(source_window_ != nullptr ? source_window_ : GetForegroundWindow(), MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{ sizeof(MONITORINFO) };
        GetMonitorInfoW(monitor, &info);

        const UINT dpi = source_window_ != nullptr ? GetDpiForWindow(source_window_) : 96;
        const int desired_width = MulDiv(files_.size() > 1 ? 920 : 720, static_cast<int>(dpi), 96);
        const int desired_height = MulDiv(520, static_cast<int>(dpi), 96);
        const int work_width = info.rcWork.right - info.rcWork.left;
        const int work_height = info.rcWork.bottom - info.rcWork.top;
        const int width = std::min(desired_width, work_width);
        const int height = std::min(desired_height, work_height);
        const int x = info.rcWork.left + (work_width - width) / 2;
        const int y = info.rcWork.top + (work_height - height) / 2;

        SetWindowPos(
            window_,
            topmost_ ? HWND_TOPMOST : HWND_TOP,
            x,
            y,
            width,
            height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
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
        OpenDefaultButton().Visibility(from_explorer ? Visibility::Collapsed : Visibility::Visible);

        if (file.is_cloud_placeholder || file.path.empty())
        {
            present_generic(file);
            return;
        }

        const auto kind = glance::app::resolve_preview_kind(file.path);
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
            ImageTransform().Rotation(image_rotation_);
            ImagePreview().Source(nullptr);
            ImageMetadataOverlay().Visibility(Visibility::Collapsed);
            static_cast<void>(ImageScroller().ChangeView(nullptr, nullptr, 1.0F, true));
            load_image_async(file.path, generation);
            break;
        case glance::app::PreviewKind::media:
            show_content_panel(kind);
            load_media_async(file.path, generation);
            break;
        case glance::app::PreviewKind::pdf:
            show_content_panel(kind);
            PdfPageText().Text(L"Loading...");
            load_pdf_async(file.path, generation);
            break;
        case glance::app::PreviewKind::archive:
            if (CompareStringOrdinal(
                    std::filesystem::path(file.path).extension().c_str(),
                    -1,
                    L".zip",
                    -1,
                    TRUE) == CSTR_EQUAL)
            {
                show_content_panel(kind);
                ArchiveStatusText().Text(L"Loading directory...");
                ArchiveEntryList().Items().Clear();
                load_archive_async(file.path, generation);
            }
            else
            {
                present_generic(file);
            }
            break;
        case glance::app::PreviewKind::office:
            show_content_panel(glance::app::PreviewKind::pdf);
            PdfPageText().Text(L"Converting with Microsoft Office...");
            load_office_async(file.path, generation);
            break;
        default:
            present_generic(file);
            break;
        }
    }

    void MainWindow::present_generic(const glance::app::PreviewFile& file)
    {
        show_content_panel(glance::app::PreviewKind::generic);
        FileNameText().Text(file.display_name);
        FilePathText().Text(!file.path.empty() ? file.path : file.parsing_name);
        FileMetadataText().Text(formatted_size(file.size) + L"  |  " + formatted_time(file.last_write_time));
        LoadCloudFileButton().Visibility(file.is_cloud_placeholder ? Visibility::Visible : Visibility::Collapsed);
        ErrorText().Visibility(Visibility::Collapsed);
    }

    void MainWindow::present_text(const glance::app::PreviewFile& file, bool markdown)
    {
        show_content_panel(markdown ? glance::app::PreviewKind::markdown : glance::app::PreviewKind::text);
        current_text_.clear();
        markdown_preview_ = markdown;
        TextPreviewBox().Text(L"Loading...");
        TextPreviewBox().Visibility(markdown ? Visibility::Collapsed : Visibility::Visible);
        MarkdownPreviewScroller().Visibility(markdown ? Visibility::Visible : Visibility::Collapsed);
        MarkdownModeButton().Visibility(markdown ? Visibility::Visible : Visibility::Collapsed);
        MarkdownModeButton().Content(box_value(markdown ? L"Code" : L"Preview"));
        MarkdownPreviewText().Blocks().Clear();
        load_text_async(file.path, markdown, content_generation_);
    }

    fire_and_forget MainWindow::load_text_async(
        std::wstring path,
        bool markdown,
        std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        co_await resume_background();
        auto preview = glance::app::load_text_preview(path);
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
            const std::wstring camera = std::wstring(properties.CameraManufacturer()) +
                (properties.CameraModel().empty() ? L"" : L" " + std::wstring(properties.CameraModel()));
            static_cast<void>(dispatcher.TryEnqueue([lifetime, bitmap, generation, width, height, camera] {
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
                        lifetime->formatted_size(current.size) + L"  |  " +
                        std::to_wstring(width) + L" x " + std::to_wstring(height));
                }
                lifetime->ImageMetadataText().Text(
                    std::to_wstring(width) + L" x " + std::to_wstring(height) +
                    (camera.empty() ? L"" : L"\n" + camera));
                lifetime->ImageMetadataOverlay().Visibility(Visibility::Visible);
            }));
        }
        catch (const hresult_error& error)
        {
            const std::wstring message = L"The image could not be decoded. " + std::wstring(error.message());
            static_cast<void>(dispatcher.TryEnqueue([lifetime, message, generation] {
                lifetime->show_provider_error(message, generation);
            }));
        }
    }

    fire_and_forget MainWindow::load_media_async(std::wstring path, std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        try
        {
            const auto file = co_await Windows::Storage::StorageFile::GetFileFromPathAsync(path);
            const auto source = Windows::Media::Core::MediaSource::CreateFromStorageFile(file);
            static_cast<void>(dispatcher.TryEnqueue([lifetime, source, generation] {
                if (generation == lifetime->content_generation_)
                {
                    lifetime->MediaPreview().Source(source);
                }
            }));
        }
        catch (const hresult_error& error)
        {
            const std::wstring message = L"The media file could not be opened. " + std::wstring(error.message());
            static_cast<void>(dispatcher.TryEnqueue([lifetime, message, generation] {
                lifetime->show_provider_error(message, generation);
            }));
        }
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
                show_provider_error(L"The PDF contains no pages.", generation);
                co_return;
            }
            render_pdf_page_async(pdf_page_index_, generation);
        }
        catch (const hresult_error&)
        {
            show_provider_error(
                L"The PDF could not be opened. Protected PDF files are not supported.",
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
            PdfPageText().Text(
                std::to_wstring(page_index + 1) + L" / " + std::to_wstring(pdf_document_.PageCount()));
        }
        catch (const hresult_error& error)
        {
            show_provider_error(
                L"The PDF page could not be rendered. " + std::wstring(error.message()),
                generation);
        }
    }

    fire_and_forget MainWindow::load_archive_async(std::wstring path, std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        co_await resume_background();
        auto preview = glance::app::load_shell_archive_preview(path);
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
            std::wstring text = entry.is_folder
                ? L"[Folder]  " + entry.name
                : entry.name + L"    " + formatted_size(entry.size);
            items.Append(box_value(text));
        }
        ArchiveStatusText().Text(
            std::to_wstring(preview.entries.size()) + L" entries" +
            (preview.truncated ? L"  |  List truncated" : L""));
    }

    fire_and_forget MainWindow::load_office_async(std::wstring path, std::uint64_t generation)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = DispatcherQueue();
        co_await resume_background();

        std::error_code filesystem_error;
        const auto cache_directory = std::filesystem::temp_directory_path(filesystem_error) / L"Glance" / L"Office";
        std::filesystem::create_directories(cache_directory, filesystem_error);
        const auto output_path = cache_directory /
            (L"preview-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(instance_id_) + L"-" + std::to_wstring(generation) + L".pdf");
        const auto host_path = executable_directory() / L"Glance.OfficeHost.exe";

        DWORD exit_code = ERROR_FILE_NOT_FOUND;
        if (!filesystem_error && std::filesystem::exists(host_path))
        {
            std::wstring command_line = quote_command_line_argument(host_path.wstring()) + L" " +
                quote_command_line_argument(path) + L" " + quote_command_line_argument(output_path.wstring());
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

        const bool succeeded = exit_code == 0 && std::filesystem::is_regular_file(output_path, filesystem_error);
        static_cast<void>(dispatcher.TryEnqueue([lifetime, output = output_path.wstring(), generation, succeeded] {
            if (generation != lifetime->content_generation_)
            {
                DeleteFileW(output.c_str());
                return;
            }
            if (!succeeded)
            {
                lifetime->show_provider_error(
                    L"Microsoft Office could not convert this document to PDF.",
                    generation);
                return;
            }
            if (!lifetime->office_temp_pdf_.empty())
            {
                DeleteFileW(lifetime->office_temp_pdf_.c_str());
            }
            lifetime->office_temp_pdf_ = output;
            lifetime->PdfPageText().Text(L"Loading converted document...");
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
            TextPreviewBox().Text(preview.error);
            TextPreviewBox().Visibility(Visibility::Visible);
            MarkdownPreviewScroller().Visibility(Visibility::Collapsed);
            return;
        }

        current_text_ = std::move(preview.content);
        TextEncodingText().Text(
            preview.encoding + (preview.truncated ? L"  |  Preview truncated at 8 MB" : L""));

        std::size_t line_count = 1;
        line_count += static_cast<std::size_t>(std::ranges::count(current_text_, L'\n'));
        const auto number_width = std::to_wstring(line_count).size();
        std::wstringstream input(current_text_);
        std::wostringstream numbered;
        std::wstring line;
        std::size_t line_number = 1;
        while (std::getline(input, line))
        {
            numbered << std::setw(static_cast<int>(number_width)) << line_number++ << L"  " << line << L'\n';
        }
        TextPreviewBox().Text(numbered.str());

        if (markdown)
        {
            render_markdown();
            TextPreviewBox().Visibility(markdown_preview_ ? Visibility::Collapsed : Visibility::Visible);
            MarkdownPreviewScroller().Visibility(markdown_preview_ ? Visibility::Visible : Visibility::Collapsed);
        }
    }

    void MainWindow::render_markdown()
    {
        auto blocks = MarkdownPreviewText().Blocks();
        blocks.Clear();
        std::wistringstream input(current_text_);
        std::wstring line;
        bool code_block{};
        while (std::getline(input, line))
        {
            if (line.starts_with(L"```"))
            {
                code_block = !code_block;
                continue;
            }

            Paragraph paragraph;
            Run run;
            std::size_t heading_level{};
            while (heading_level < line.size() && heading_level < 6 && line[heading_level] == L'#')
            {
                ++heading_level;
            }
            if (heading_level > 0 && heading_level < line.size() && line[heading_level] == L' ')
            {
                line.erase(0, heading_level + 1);
                run.FontSize(28.0 - static_cast<double>(heading_level) * 2.5);
            }
            if (code_block)
            {
                run.FontFamily(Media::FontFamily(L"Cascadia Mono, Consolas"));
            }
            run.Text(line.empty() ? L" " : line);
            paragraph.Inlines().Append(run);
            blocks.Append(paragraph);
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
        if (kind != glance::app::PreviewKind::image)
        {
            ImagePreview().Source(nullptr);
            ImageMetadataOverlay().Visibility(Visibility::Collapsed);
        }
        if (kind != glance::app::PreviewKind::media)
        {
            MediaPreview().Source(nullptr);
        }
        if (kind != glance::app::PreviewKind::pdf)
        {
            pdf_document_ = nullptr;
            PdfPageImage().Source(nullptr);
            if (!office_temp_pdf_.empty())
            {
                DeleteFileW(office_temp_pdf_.c_str());
                office_temp_pdf_.clear();
            }
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

    void MainWindow::TopmostButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        topmost_ = !topmost_;
        set_topmost(topmost_);
        update_state();
    }

    void MainWindow::FitContentButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        user_sized_ = false;
        position_initial_window();
    }

    void MainWindow::MarkdownModeButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        markdown_preview_ = !markdown_preview_;
        MarkdownModeButton().Content(box_value(markdown_preview_ ? L"Code" : L"Preview"));
        TextPreviewBox().Visibility(markdown_preview_ ? Visibility::Collapsed : Visibility::Visible);
        MarkdownPreviewScroller().Visibility(markdown_preview_ ? Visibility::Visible : Visibility::Collapsed);
    }

    void MainWindow::ImagePanel_SizeChanged(IInspectable const&, SizeChangedEventArgs const&)
    {
        update_image_fit_surface();
    }

    void MainWindow::ZoomOutButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        const float zoom = std::max(1.0F, ImageScroller().ZoomFactor() / 1.25F);
        static_cast<void>(ImageScroller().ChangeView(nullptr, nullptr, zoom));
    }

    void MainWindow::ZoomInButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        const float zoom = std::min(16.0F, ImageScroller().ZoomFactor() * 1.25F);
        static_cast<void>(ImageScroller().ChangeView(nullptr, nullptr, zoom));
    }

    void MainWindow::RotateLeftButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        image_rotation_ = std::fmod(image_rotation_ - 90.0 + 360.0, 360.0);
        ImageTransform().Rotation(image_rotation_);
    }

    void MainWindow::RotateRightButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        image_rotation_ = std::fmod(image_rotation_ + 90.0, 360.0);
        ImageTransform().Rotation(image_rotation_);
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

    void MainWindow::PinButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        pinned_ = !pinned_;
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
            focus_timer_.Tick([this](IInspectable const&, IInspectable const&)
            {
                if (GetForegroundWindow() != foreground_when_unpinned_)
                {
                    stop_detached_focus_monitor();
                    Close();
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
        Windows::ApplicationModel::DataTransfer::DataPackage package;
        package.SetText(!files_[current_index_].path.empty() ? files_[current_index_].path : files_[current_index_].parsing_name);
        Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
        Windows::ApplicationModel::DataTransfer::Clipboard::Flush();
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

    void MainWindow::ConfirmCloudLoad_Click(IInspectable const&, RoutedEventArgs const&)
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
            ErrorText().Text(L"The file could not be loaded. Error " + std::to_wstring(GetLastError()) + L".");
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
            return L"Unknown time";
        }
        wchar_t date[64]{};
        wchar_t time[64]{};
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &system_time, nullptr, date, 64, nullptr);
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &system_time, nullptr, time, 64);
        return std::wstring(date) + L" " + time;
    }
}

#include "pch.h"
#include "native_preview_surface.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <ranges>
#include <utility>
#include <vector>

namespace
{
    using namespace glance::contracts::native_preview;

    constexpr wchar_t host_window_class[] = L"Glance.NativePreviewSurface";
    std::once_flag host_class_once;
    std::mutex mouse_hook_mutex;
    std::vector<glance::app::NativePreviewSurface*> mouse_hook_surfaces;
    HHOOK mouse_hook{};

    LRESULT CALLBACK mouse_hook_proc(int code, WPARAM wparam, LPARAM lparam) noexcept
    {
        if (code >= 0 && lparam != 0)
        {
            const auto* input = reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam);
            std::scoped_lock lock(mouse_hook_mutex);
            for (auto* surface : mouse_hook_surfaces)
            {
                if (surface != nullptr && surface->handle_mouse_message(wparam, input->pt))
                {
                    return 1;
                }
            }
        }
        return CallNextHookEx(mouse_hook, code, wparam, lparam);
    }

    bool read_exact(HANDLE handle, void* destination, std::size_t size) noexcept
    {
        auto* bytes = static_cast<std::byte*>(destination);
        while (size != 0)
        {
            DWORD read{};
            const auto request = static_cast<DWORD>(
                std::min<std::size_t>(size, MAXDWORD));
            if (!ReadFile(handle, bytes, request, &read, nullptr) || read == 0)
            {
                return false;
            }
            bytes += read;
            size -= read;
        }
        return true;
    }

    bool write_exact(HANDLE handle, const void* source, std::size_t size) noexcept
    {
        const auto* bytes = static_cast<const std::byte*>(source);
        while (size != 0)
        {
            DWORD written{};
            const auto request = static_cast<DWORD>(
                std::min<std::size_t>(size, MAXDWORD));
            if (!WriteFile(handle, bytes, request, &written, nullptr) || written == 0)
            {
                return false;
            }
            bytes += written;
            size -= written;
        }
        return true;
    }

    std::wstring handle_argument(HANDLE handle)
    {
        return std::to_wstring(reinterpret_cast<std::uintptr_t>(handle));
    }

    void terminate_after_grace_period(HANDLE process)
    {
        std::thread([process] {
            if (WaitForSingleObject(process, 2000) == WAIT_TIMEOUT)
            {
                static_cast<void>(TerminateProcess(process, ERROR_CANCELLED));
            }
            CloseHandle(process);
        }).detach();
    }

    class ProcessTimeout final
    {
    public:
        ProcessTimeout(HANDLE process, DWORD timeout_ms) noexcept
        {
            if (process == nullptr ||
                !DuplicateHandle(
                    GetCurrentProcess(),
                    process,
                    GetCurrentProcess(),
                    &process_,
                    PROCESS_TERMINATE,
                    FALSE,
                    0))
            {
                return;
            }
            timer_ = CreateThreadpoolTimer(timeout_callback, this, nullptr);
            if (timer_ == nullptr)
            {
                CloseHandle(std::exchange(process_, nullptr));
                return;
            }
            LARGE_INTEGER due_time{};
            due_time.QuadPart = -static_cast<LONGLONG>(timeout_ms) * 10000LL;
            FILETIME due_file_time{
                due_time.LowPart,
                static_cast<DWORD>(due_time.HighPart) };
            SetThreadpoolTimer(timer_, &due_file_time, 0, 0);
        }

        ~ProcessTimeout()
        {
            if (timer_ != nullptr)
            {
                SetThreadpoolTimer(timer_, nullptr, 0, 0);
                WaitForThreadpoolTimerCallbacks(timer_, TRUE);
                CloseThreadpoolTimer(timer_);
            }
            if (process_ != nullptr)
            {
                CloseHandle(process_);
            }
        }

        ProcessTimeout(const ProcessTimeout&) = delete;
        ProcessTimeout& operator=(const ProcessTimeout&) = delete;

    private:
        static void CALLBACK timeout_callback(
            PTP_CALLBACK_INSTANCE,
            void* context,
            PTP_TIMER) noexcept
        {
            const auto timeout = static_cast<ProcessTimeout*>(context);
            static_cast<void>(TerminateProcess(timeout->process_, ERROR_TIMEOUT));
        }

        HANDLE process_{};
        PTP_TIMER timer_{};
    };

    void register_host_window_class(HINSTANCE instance)
    {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = glance::app::NativePreviewSurface::host_window_proc;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.lpszClassName = host_window_class;
        RegisterClassExW(&window_class);
    }
}

namespace glance::app
{
    NativePreviewSurface::NativePreviewSurface(
        HWND parent,
        std::wstring host_path,
        std::shared_ptr<void> renderer_lease,
        DoubleClickCallback double_click_callback)
        : parent_(parent),
          surface_thread_id_(GetCurrentThreadId()),
          host_path_(std::move(host_path)),
          renderer_lease_(std::move(renderer_lease)),
          double_click_callback_(std::move(double_click_callback))
    {
        const auto instance = GetModuleHandleW(nullptr);
        std::call_once(host_class_once, register_host_window_class, instance);
        host_ = CreateWindowExW(
            WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            host_window_class,
            nullptr,
            WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0,
            0,
            0,
            0,
            parent_,
            nullptr,
            instance,
            this);
        if (host_ != nullptr)
        {
            content_ = CreateWindowExW(
                WS_EX_NOPARENTNOTIFY,
                host_window_class,
                nullptr,
                WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                0,
                0,
                0,
                0,
                host_,
                nullptr,
                instance,
                this);
            if (content_ == nullptr)
            {
                DestroyWindow(std::exchange(host_, nullptr));
            }
        }
    }

    NativePreviewSurface::~NativePreviewSurface()
    {
        visible_ = false;
        double_click_enabled_ = false;
        update_mouse_hook_registration();
        cancel();
        {
            std::scoped_lock lock(io_mutex_);
            close_process_locked(false);
        }
        if (host_ != nullptr && IsWindow(host_))
        {
            if (GetCurrentThreadId() == surface_thread_id_)
            {
                DestroyWindow(host_);
            }
            else
            {
                PostMessageW(host_, WM_CLOSE, 0, 0);
            }
        }
    }

    bool NativePreviewSurface::available() const noexcept
    {
        return host_ != nullptr && content_ != nullptr;
    }

    bool NativePreviewSurface::start_host() noexcept
    {
        std::scoped_lock lock(io_mutex_);
        return !cancelled_.load(std::memory_order_acquire) &&
            start_process_locked();
    }

    Status NativePreviewSurface::open(
        const std::wstring& path,
        const PreviewVisuals& visuals,
        std::uint32_t dpi)
    {
        if (path.size() >
                (maximum_payload_size - sizeof(OpenRequest)) / sizeof(wchar_t) ||
            cancelled_.load(std::memory_order_acquire))
        {
            return Status::invalid_request;
        }

        std::scoped_lock lock(io_mutex_);
        if (cancelled_.load(std::memory_order_acquire) || !start_process_locked())
        {
            return Status::cancelled;
        }

        background_.store(RGB(
            visuals.background_color & 0xFFU,
            (visuals.background_color >> 8U) & 0xFFU,
            (visuals.background_color >> 16U) & 0xFFU),
            std::memory_order_release);
        OpenRequest request{
            .parent_window = reinterpret_cast<std::uint64_t>(content_),
            .bounds = { 0, 0, static_cast<std::int32_t>(width_),
                static_cast<std::int32_t>(height_) },
            .visuals = visuals,
            .dpi = dpi,
            .path_characters = static_cast<std::uint32_t>(path.size()) };
        std::vector<std::byte> payload(sizeof(request) + path.size() * sizeof(wchar_t));
        std::memcpy(payload.data(), &request, sizeof(request));
        std::memcpy(
            payload.data() + sizeof(request),
            path.data(),
            path.size() * sizeof(wchar_t));

        for (int attempt = 0; attempt != 2; ++attempt)
        {
            Status status{ Status::open_failed };
            if (transact_locked(
                    Command::open_document,
                    payload.data(),
                    static_cast<std::uint32_t>(payload.size()),
                    status,
                    10000))
            {
                return status;
            }
            if (cancelled_.load(std::memory_order_acquire))
            {
                return Status::cancelled;
            }
            close_process_locked(true);
            if (attempt != 0 || !start_process_locked())
            {
                return Status::open_failed;
            }
        }
        return Status::open_failed;
    }

    void NativePreviewSurface::resize(
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t dpi) noexcept
    {
        if (cancelled_.load(std::memory_order_acquire))
        {
            return;
        }
        const ResizeRequest request{
            .bounds = { 0, 0, static_cast<std::int32_t>(width),
                static_cast<std::int32_t>(height) },
            .dpi = dpi };
        std::scoped_lock lock(io_mutex_);
        Status status{};
        static_cast<void>(transact_locked(
            Command::resize,
            &request,
            sizeof(request),
            status,
            2000));
    }

    void NativePreviewSurface::set_visuals(const PreviewVisuals& visuals) noexcept
    {
        background_.store(RGB(
            visuals.background_color & 0xFFU,
            (visuals.background_color >> 8U) & 0xFFU,
            (visuals.background_color >> 16U) & 0xFFU),
            std::memory_order_release);
        if (host_ != nullptr)
        {
            InvalidateRect(host_, nullptr, TRUE);
        }
        if (cancelled_.load(std::memory_order_acquire))
        {
            return;
        }
        std::scoped_lock lock(io_mutex_);
        Status status{};
        static_cast<void>(transact_locked(
            Command::set_visuals,
            &visuals,
            sizeof(visuals),
            status,
            2000));
    }

    void NativePreviewSurface::set_bounds(
        int x,
        int y,
        int width,
        int height) noexcept
    {
        width_ = static_cast<std::uint32_t>(std::max(0, width));
        height_ = static_cast<std::uint32_t>(std::max(0, height));
        if (host_ == nullptr)
        {
            return;
        }
        POINT origin{ x, y };
        ClientToScreen(parent_, &origin);
        SetWindowPos(
            host_,
            HWND_TOP,
            origin.x,
            origin.y,
            static_cast<int>(width_),
            static_cast<int>(height_),
            SWP_NOACTIVATE | (visible_ ? SWP_SHOWWINDOW : 0));
        if (content_ != nullptr)
        {
            SetWindowPos(
                content_,
                nullptr,
                0,
                0,
                static_cast<int>(width_),
                static_cast<int>(height_),
                SWP_NOACTIVATE | SWP_NOZORDER);
        }
    }

    void NativePreviewSurface::set_occlusions(
        std::span<const RECT> rectangles) noexcept
    {
        if (host_ == nullptr)
        {
            return;
        }
        if (rectangles.empty())
        {
            SetWindowRgn(host_, nullptr, TRUE);
            return;
        }
        RECT bounds{};
        if (!GetClientRect(host_, &bounds))
        {
            return;
        }
        const HRGN visible_region = CreateRectRgnIndirect(&bounds);
        if (visible_region == nullptr)
        {
            return;
        }
        for (const auto& rectangle : rectangles)
        {
            const HRGN excluded = CreateRectRgnIndirect(&rectangle);
            if (excluded != nullptr)
            {
                CombineRgn(visible_region, visible_region, excluded, RGN_DIFF);
                DeleteObject(excluded);
            }
        }
        SetWindowRgn(host_, visible_region, TRUE);
    }

    void NativePreviewSurface::set_visible(bool visible) noexcept
    {
        visible_ = visible;
        if (host_ != nullptr)
        {
            ShowWindow(host_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
        }
        update_mouse_hook_registration();
    }

    void NativePreviewSurface::set_double_click_enabled(bool enabled) noexcept
    {
        double_click_enabled_ = enabled;
        update_mouse_hook_registration();
    }

    void NativePreviewSurface::cancel() noexcept
    {
        if (cancelled_.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }
        HANDLE process_copy{};
        {
            std::scoped_lock lock(process_mutex_);
            if (cancellation_event_ != nullptr)
            {
                SetEvent(cancellation_event_);
            }
            if (process_ != nullptr)
            {
                static_cast<void>(DuplicateHandle(
                    GetCurrentProcess(),
                    process_,
                    GetCurrentProcess(),
                    &process_copy,
                    SYNCHRONIZE | PROCESS_TERMINATE,
                    FALSE,
                    0));
            }
        }
        if (process_copy != nullptr)
        {
            terminate_after_grace_period(process_copy);
        }
    }

    void NativePreviewSurface::destroy_surface() noexcept
    {
        visible_ = false;
        update_mouse_hook_registration();
        if (host_ != nullptr && IsWindow(host_))
        {
            ShowWindow(host_, SW_HIDE);
            DestroyWindow(host_);
        }
        host_ = nullptr;
        content_ = nullptr;
    }

    void NativePreviewSurface::shutdown() noexcept
    {
        std::scoped_lock lock(io_mutex_);
        if (process_ != nullptr && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT)
        {
            Status status{};
            static_cast<void>(transact_locked(
                Command::shutdown,
                nullptr,
                0,
                status,
                cancelled_.load(std::memory_order_acquire) ? 2000 : 5000));
        }
        close_process_locked(false);
    }

    bool NativePreviewSurface::start_process_locked()
    {
        if (process_ != nullptr && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT)
        {
            return true;
        }
        close_process_locked(false);
        std::error_code error;
        if (!std::filesystem::is_regular_file(host_path_, error))
        {
            return false;
        }

        SECURITY_ATTRIBUTES security{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        HANDLE child_request_read{};
        HANDLE parent_request_write{};
        HANDLE parent_response_read{};
        HANDLE child_response_write{};
        HANDLE cancellation_event = CreateEventW(&security, TRUE, FALSE, nullptr);
        if (cancellation_event == nullptr ||
            !CreatePipe(&child_request_read, &parent_request_write, &security, 0) ||
            !CreatePipe(&parent_response_read, &child_response_write, &security, 0))
        {
            if (child_request_read != nullptr) CloseHandle(child_request_read);
            if (parent_request_write != nullptr) CloseHandle(parent_request_write);
            if (parent_response_read != nullptr) CloseHandle(parent_response_read);
            if (child_response_write != nullptr) CloseHandle(child_response_write);
            if (cancellation_event != nullptr)
            {
                CloseHandle(cancellation_event);
            }
            return false;
        }
        if (!SetHandleInformation(parent_request_write, HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(parent_response_read, HANDLE_FLAG_INHERIT, 0))
        {
            CloseHandle(child_request_read);
            CloseHandle(parent_request_write);
            CloseHandle(parent_response_read);
            CloseHandle(child_response_write);
            CloseHandle(cancellation_event);
            return false;
        }

        std::wstring command = L"\"" + host_path_ + L"\" " +
            handle_argument(child_request_read) + L" " +
            handle_argument(child_response_write) + L" " +
            handle_argument(cancellation_event);
        SIZE_T attribute_bytes{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
        std::vector<std::byte> attribute_storage(attribute_bytes);
        auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(
            attribute_storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_bytes))
        {
            CloseHandle(child_request_read);
            CloseHandle(parent_request_write);
            CloseHandle(parent_response_read);
            CloseHandle(child_response_write);
            CloseHandle(cancellation_event);
            return false;
        }
        std::array inherited_handles{
            child_request_read,
            child_response_write,
            cancellation_event };
        if (!UpdateProcThreadAttribute(
                attributes,
                0,
                PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                inherited_handles.data(),
                sizeof(inherited_handles),
                nullptr,
                nullptr))
        {
            DeleteProcThreadAttributeList(attributes);
            CloseHandle(child_request_read);
            CloseHandle(parent_request_write);
            CloseHandle(parent_response_read);
            CloseHandle(child_response_write);
            CloseHandle(cancellation_event);
            return false;
        }

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(
            host_path_.c_str(),
            command.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
            nullptr,
            nullptr,
            &startup.StartupInfo,
            &process);
        DeleteProcThreadAttributeList(attributes);
        CloseHandle(child_request_read);
        CloseHandle(child_response_write);
        if (!created)
        {
            CloseHandle(parent_request_write);
            CloseHandle(parent_response_read);
            CloseHandle(cancellation_event);
            return false;
        }
        CloseHandle(process.hThread);
        HANDLE process_copy{};
        {
            std::scoped_lock lock(process_mutex_);
            process_ = process.hProcess;
            request_pipe_ = parent_request_write;
            response_pipe_ = parent_response_read;
            cancellation_event_ = cancellation_event;
            if (cancelled_.load(std::memory_order_acquire))
            {
                SetEvent(cancellation_event_);
                static_cast<void>(DuplicateHandle(
                    GetCurrentProcess(),
                    process_,
                    GetCurrentProcess(),
                    &process_copy,
                    SYNCHRONIZE | PROCESS_TERMINATE,
                    FALSE,
                    0));
            }
        }
        if (process_copy != nullptr)
        {
            terminate_after_grace_period(process_copy);
        }
        return true;
    }

    bool NativePreviewSurface::transact_locked(
        Command command,
        const void* payload,
        std::uint32_t payload_size,
        Status& status,
        DWORD timeout_ms) noexcept
    {
        if (process_ == nullptr || request_pipe_ == nullptr ||
            response_pipe_ == nullptr || payload_size > maximum_payload_size)
        {
            return false;
        }
        const RequestHeader request{
            .command = command,
            .payload_size = payload_size };
        ProcessTimeout timeout(process_, timeout_ms);
        if (!write_exact(request_pipe_, &request, sizeof(request)) ||
            (payload_size != 0 && !write_exact(request_pipe_, payload, payload_size)))
        {
            return false;
        }
        ResponseHeader response;
        if (!read_exact(response_pipe_, &response, sizeof(response)) ||
            response.magic != protocol_magic ||
            response.version != protocol_version ||
            response.payload_size > maximum_payload_size)
        {
            return false;
        }
        if (response.payload_size != 0)
        {
            std::vector<std::byte> ignored(response.payload_size);
            if (!read_exact(response_pipe_, ignored.data(), ignored.size()))
            {
                return false;
            }
        }
        status = response.status;
        return true;
    }

    void NativePreviewSurface::close_process_locked(bool terminate) noexcept
    {
        HANDLE process{};
        HANDLE cancellation_event{};
        {
            std::scoped_lock lock(process_mutex_);
            process = std::exchange(process_, nullptr);
            cancellation_event = std::exchange(cancellation_event_, nullptr);
        }
        if (process != nullptr)
        {
            if (terminate && WaitForSingleObject(process, 0) == WAIT_TIMEOUT)
            {
                TerminateProcess(process, ERROR_CANCELLED);
                WaitForSingleObject(process, 1000);
            }
            CloseHandle(process);
        }
        if (request_pipe_ != nullptr)
        {
            CloseHandle(std::exchange(request_pipe_, nullptr));
        }
        if (response_pipe_ != nullptr)
        {
            CloseHandle(std::exchange(response_pipe_, nullptr));
        }
        if (cancellation_event != nullptr)
        {
            CloseHandle(cancellation_event);
        }
    }

    bool NativePreviewSurface::handle_mouse_message(
        WPARAM message,
        const POINT& point) noexcept
    {
        if (!visible_ || !double_click_enabled_ || host_ == nullptr)
        {
            return false;
        }
        RECT bounds{};
        if (!GetWindowRect(host_, &bounds) || !PtInRect(&bounds, point))
        {
            return false;
        }
        if (message == WM_LBUTTONUP && suppress_next_left_up_)
        {
            suppress_next_left_up_ = false;
            return true;
        }
        if (message != WM_LBUTTONDOWN)
        {
            return false;
        }

        const ULONGLONG now = GetTickCount64();
        const int maximum_x = std::max(1, GetSystemMetrics(SM_CXDOUBLECLK) / 2);
        const int maximum_y = std::max(1, GetSystemMetrics(SM_CYDOUBLECLK) / 2);
        const bool double_click = last_left_down_tick_ != 0 &&
            now - last_left_down_tick_ <= GetDoubleClickTime() &&
            std::abs(point.x - last_left_down_point_.x) <= maximum_x &&
            std::abs(point.y - last_left_down_point_.y) <= maximum_y;
        last_left_down_tick_ = now;
        last_left_down_point_ = point;
        if (!double_click)
        {
            return false;
        }
        last_left_down_tick_ = 0;
        suppress_next_left_up_ = true;
        if (double_click_callback_)
        {
            double_click_callback_();
        }
        return true;
    }

    void NativePreviewSurface::update_mouse_hook_registration() noexcept
    {
        const bool should_register = visible_ && double_click_enabled_ && host_ != nullptr;
        std::scoped_lock lock(mouse_hook_mutex);
        if (should_register == mouse_hook_registered_)
        {
            return;
        }
        if (should_register)
        {
            if (mouse_hook == nullptr)
            {
                mouse_hook = SetWindowsHookExW(
                    WH_MOUSE_LL,
                    mouse_hook_proc,
                    GetModuleHandleW(nullptr),
                    0);
                if (mouse_hook == nullptr)
                {
                    return;
                }
            }
            mouse_hook_surfaces.push_back(this);
            mouse_hook_registered_ = true;
            return;
        }
        std::erase(mouse_hook_surfaces, this);
        mouse_hook_registered_ = false;
        if (mouse_hook_surfaces.empty() && mouse_hook != nullptr)
        {
            UnhookWindowsHookEx(std::exchange(mouse_hook, nullptr));
        }
    }

    LRESULT CALLBACK NativePreviewSurface::host_window_proc(
        HWND window,
        UINT message,
        WPARAM wparam,
        LPARAM lparam) noexcept
    {
        auto* self = reinterpret_cast<NativePreviewSurface*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            const auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<NativePreviewSurface*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (message == WM_ERASEBKGND && self != nullptr)
        {
            RECT bounds{};
            GetClientRect(window, &bounds);
            const HBRUSH brush = CreateSolidBrush(
                self->background_.load(std::memory_order_acquire));
            FillRect(reinterpret_cast<HDC>(wparam), &bounds, brush);
            DeleteObject(brush);
            return 1;
        }
        if (message == WM_CLOSE)
        {
            DestroyWindow(window);
            return 0;
        }
        if (message == WM_NCDESTROY && self != nullptr && window == self->content_)
        {
            self->content_ = nullptr;
        }
        if (message == WM_NCDESTROY && self != nullptr && window == self->host_)
        {
            self->host_ = nullptr;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

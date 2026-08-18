#include "../include/preview_handler_host.h"

#include "glance/contracts/native_preview_protocol.h"

#include <shobjidl_core.h>
#include <shlwapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <winrt/base.h>

namespace
{
    using namespace glance::contracts::native_preview;

    constexpr wchar_t preview_handler_key[] =
        L"shellex\\{8895b1c6-b41f-4c1c-a562-0d564250836f}";
    constexpr UINT request_message = WM_APP + 1;

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

    bool read_registry_default(std::wstring_view subkey, std::wstring& value)
    {
        HKEY key{};
        if (RegOpenKeyExW(
                HKEY_CLASSES_ROOT,
                std::wstring(subkey).c_str(),
                0,
                KEY_QUERY_VALUE,
                &key) != ERROR_SUCCESS)
        {
            return false;
        }
        std::array<wchar_t, 512> buffer{};
        DWORD type{};
        DWORD bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
        const auto status = RegQueryValueExW(
            key,
            nullptr,
            nullptr,
            &type,
            reinterpret_cast<BYTE*>(buffer.data()),
            &bytes);
        RegCloseKey(key);
        if (status != ERROR_SUCCESS ||
            (type != REG_SZ && type != REG_EXPAND_SZ) ||
            bytes <= sizeof(wchar_t))
        {
            return false;
        }
        buffer.back() = L'\0';
        value.assign(buffer.data());
        return !value.empty();
    }

    std::optional<CLSID> preview_handler_for_path(const std::wstring& path)
    {
        auto extension = std::filesystem::path(path).extension().wstring();
        if (extension.empty())
        {
            return std::nullopt;
        }
        std::wstring class_id;
        if (!read_registry_default(
                extension + L"\\" + preview_handler_key,
                class_id))
        {
            std::wstring programmatic_id;
            if (!read_registry_default(extension, programmatic_id) ||
                !read_registry_default(
                    programmatic_id + L"\\" + preview_handler_key,
                    class_id))
            {
                return std::nullopt;
            }
        }
        CLSID value{};
        return SUCCEEDED(CLSIDFromString(class_id.c_str(), &value))
            ? std::optional<CLSID>(value)
            : std::nullopt;
    }

    class PreviewHandlerFrame :
        public winrt::implements<PreviewHandlerFrame, IPreviewHandlerFrame>
    {
    public:
        HRESULT STDMETHODCALLTYPE GetWindowContext(
            PREVIEWHANDLERFRAMEINFO* info) noexcept override
        {
            if (info == nullptr)
            {
                return E_POINTER;
            }
            *info = {};
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE TranslateAccelerator(MSG*) noexcept override
        {
            return S_FALSE;
        }
    };

    class PreviewHandlerSession final
    {
    public:
        Status open(
            const std::wstring& path,
            HWND parent,
            const RECT& bounds,
            const PreviewVisuals& visuals,
            HANDLE cancellation_event)
        {
            unload();
            std::error_code error;
            const auto class_id = preview_handler_for_path(path);
            if (!class_id.has_value())
            {
                return Status::handler_missing;
            }
            if (parent == nullptr || !IsWindow(parent) ||
                !std::filesystem::path(path).is_absolute() ||
                !std::filesystem::is_regular_file(path, error))
            {
                return Status::invalid_request;
            }
            if (WaitForSingleObject(cancellation_event, 0) == WAIT_OBJECT_0)
            {
                return Status::cancelled;
            }

            for (int attempt = 0; attempt != 2; ++attempt)
            {
                winrt::com_ptr<IPreviewHandler> handler;
                HRESULT status = CoCreateInstance(
                    *class_id,
                    nullptr,
                    CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER,
                    __uuidof(IPreviewHandler),
                    handler.put_void());
                Status failure = Status::handler_creation_failed;
                if (SUCCEEDED(status))
                {
                    const auto initialize = handler.try_as<IInitializeWithFile>();
                    status = initialize == nullptr
                        ? E_NOINTERFACE
                        : initialize->Initialize(path.c_str(), STGM_READ);
                    failure = Status::initialization_failed;
                }

                auto frame = winrt::make_self<PreviewHandlerFrame>();
                const auto site = handler == nullptr
                    ? winrt::com_ptr<IObjectWithSite>{}
                    : handler.try_as<IObjectWithSite>();
                const auto visual_handler = handler == nullptr
                    ? winrt::com_ptr<IPreviewHandlerVisuals>{}
                    : handler.try_as<IPreviewHandlerVisuals>();
                if (SUCCEEDED(status))
                {
                    if (site != nullptr)
                    {
                        static_cast<void>(site->SetSite(frame.get()));
                    }
                    if (visual_handler != nullptr)
                    {
                        static_cast<void>(visual_handler->SetBackgroundColor(
                            static_cast<COLORREF>(visuals.background_color)));
                        static_cast<void>(visual_handler->SetTextColor(
                            static_cast<COLORREF>(visuals.text_color)));
                    }
                    status = handler->SetWindow(parent, &bounds);
                    failure = Status::window_binding_failed;
                }
                if (SUCCEEDED(status))
                {
                    status = handler->DoPreview();
                    failure = Status::preview_failed;
                }
                if (SUCCEEDED(status))
                {
                    if (WaitForSingleObject(cancellation_event, 0) == WAIT_OBJECT_0)
                    {
                        static_cast<void>(handler->Unload());
                        if (site != nullptr)
                        {
                            static_cast<void>(site->SetSite(nullptr));
                        }
                        return Status::cancelled;
                    }
                    handler_ = std::move(handler);
                    site_ = std::move(site);
                    frame_ = std::move(frame);
                    bounds_ = bounds;
                    visuals_ = visual_handler;
                    return Status::success;
                }

                if (handler != nullptr)
                {
                    static_cast<void>(handler->Unload());
                }
                if (site != nullptr)
                {
                    static_cast<void>(site->SetSite(nullptr));
                }
                if (WaitForSingleObject(cancellation_event, attempt == 0 ? 100 : 0) !=
                    WAIT_TIMEOUT)
                {
                    return Status::cancelled;
                }
                if (attempt != 0)
                {
                    return failure;
                }
            }
            return Status::open_failed;
        }

        Status resize(const RECT& bounds)
        {
            if (handler_ == nullptr)
            {
                return Status::invalid_request;
            }
            bounds_ = bounds;
            return SUCCEEDED(handler_->SetRect(&bounds_))
                ? Status::success
                : Status::open_failed;
        }

        Status set_visuals(const PreviewVisuals& visuals)
        {
            if (handler_ == nullptr)
            {
                return Status::invalid_request;
            }
            if (visuals_ == nullptr)
            {
                return Status::success;
            }
            const HRESULT background = visuals_->SetBackgroundColor(
                static_cast<COLORREF>(visuals.background_color));
            const HRESULT text = visuals_->SetTextColor(
                static_cast<COLORREF>(visuals.text_color));
            return SUCCEEDED(background) && SUCCEEDED(text)
                ? Status::success
                : Status::open_failed;
        }

        void unload() noexcept
        {
            if (handler_ != nullptr)
            {
                static_cast<void>(handler_->Unload());
            }
            if (site_ != nullptr)
            {
                static_cast<void>(site_->SetSite(nullptr));
            }
            visuals_ = nullptr;
            site_ = nullptr;
            frame_ = nullptr;
            handler_ = nullptr;
            bounds_ = {};
        }

        ~PreviewHandlerSession()
        {
            unload();
        }

    private:
        winrt::com_ptr<IPreviewHandler> handler_;
        winrt::com_ptr<IObjectWithSite> site_;
        winrt::com_ptr<IPreviewHandlerVisuals> visuals_;
        winrt::com_ptr<PreviewHandlerFrame> frame_;
        RECT bounds_{};
    };

    struct QueuedRequest
    {
        RequestHeader header;
        std::vector<std::byte> payload;
    };

    class RequestQueue final
    {
    public:
        void push(QueuedRequest request)
        {
            std::scoped_lock lock(mutex_);
            requests_.push_back(std::move(request));
        }

        std::optional<QueuedRequest> pop()
        {
            std::scoped_lock lock(mutex_);
            if (requests_.empty())
            {
                return std::nullopt;
            }
            auto request = std::move(requests_.front());
            requests_.pop_front();
            return request;
        }

    private:
        std::mutex mutex_;
        std::deque<QueuedRequest> requests_;
    };

    Status process_request(
        const QueuedRequest& queued,
        PreviewHandlerSession& session,
        HANDLE cancellation_event)
    {
        if (WaitForSingleObject(cancellation_event, 0) == WAIT_OBJECT_0 &&
            queued.header.command != Command::shutdown)
        {
            return Status::cancelled;
        }
        switch (queued.header.command)
        {
        case Command::open_document:
        {
            if (queued.payload.size() < sizeof(OpenRequest))
            {
                return Status::invalid_request;
            }
            const auto* request = reinterpret_cast<const OpenRequest*>(
                queued.payload.data());
            const std::size_t path_bytes =
                static_cast<std::size_t>(request->path_characters) * sizeof(wchar_t);
            if (path_bytes != queued.payload.size() - sizeof(OpenRequest))
            {
                return Status::invalid_request;
            }
            const auto* path_data = reinterpret_cast<const wchar_t*>(
                queued.payload.data() + sizeof(OpenRequest));
            const std::wstring path(path_data, request->path_characters);
            const RECT bounds{
                request->bounds.left,
                request->bounds.top,
                request->bounds.right,
                request->bounds.bottom };
            return session.open(
                path,
                reinterpret_cast<HWND>(request->parent_window),
                bounds,
                request->visuals,
                cancellation_event);
        }
        case Command::resize:
        {
            if (queued.payload.size() != sizeof(ResizeRequest))
            {
                return Status::invalid_request;
            }
            const auto* request = reinterpret_cast<const ResizeRequest*>(
                queued.payload.data());
            return session.resize({
                request->bounds.left,
                request->bounds.top,
                request->bounds.right,
                request->bounds.bottom });
        }
        case Command::set_visuals:
            return queued.payload.size() == sizeof(PreviewVisuals)
                ? session.set_visuals(*reinterpret_cast<const PreviewVisuals*>(
                    queued.payload.data()))
                : Status::invalid_request;
        case Command::unload:
            session.unload();
            return Status::success;
        case Command::shutdown:
            session.unload();
            return Status::success;
        default:
            return Status::invalid_request;
        }
    }
}

namespace glance::office
{
    int run_preview_handler_host(
        HANDLE request_pipe,
        HANDLE response_pipe,
        HANDLE cancellation_event)
    {
        if (request_pipe == nullptr || response_pipe == nullptr ||
            cancellation_event == nullptr)
        {
            return 2;
        }
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        static_cast<void>(CoEnableCallCancellation(nullptr));
        const DWORD sta_thread = GetCurrentThreadId();
        MSG initial_message{};
        PeekMessageW(&initial_message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

        RequestQueue queue;
        std::atomic_bool stopped{};
        std::thread reader([&] {
            while (!stopped.load(std::memory_order_acquire))
            {
                QueuedRequest request;
                if (!read_exact(request_pipe, &request.header, sizeof(request.header)) ||
                    request.header.magic != protocol_magic ||
                    request.header.version != protocol_version ||
                    request.header.payload_size > maximum_payload_size)
                {
                    break;
                }
                request.payload.resize(request.header.payload_size);
                if (!request.payload.empty() &&
                    !read_exact(request_pipe, request.payload.data(), request.payload.size()))
                {
                    break;
                }
                queue.push(std::move(request));
                PostThreadMessageW(sta_thread, request_message, 0, 0);
            }
            PostThreadMessageW(sta_thread, WM_QUIT, 0, 0);
        });
        std::thread cancellation([&] {
            if (WaitForSingleObject(cancellation_event, INFINITE) == WAIT_OBJECT_0 &&
                !stopped.load(std::memory_order_acquire))
            {
                static_cast<void>(CoCancelCall(sta_thread, 0));
                PostThreadMessageW(sta_thread, request_message, 0, 0);
            }
        });

        PreviewHandlerSession session;
        bool running = true;
        MSG message{};
        while (running && GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            if (message.message != request_message)
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
                continue;
            }
            while (const auto request = queue.pop())
            {
                const auto status = process_request(*request, session, cancellation_event);
                const ResponseHeader response{ .status = status };
                if (!write_exact(response_pipe, &response, sizeof(response)))
                {
                    running = false;
                    break;
                }
                if (request->header.command == Command::shutdown)
                {
                    running = false;
                    break;
                }
            }
        }

        session.unload();
        stopped.store(true, std::memory_order_release);
        static_cast<void>(CancelSynchronousIo(reader.native_handle()));
        CloseHandle(request_pipe);
        SetEvent(cancellation_event);
        if (reader.joinable())
        {
            reader.join();
        }
        if (cancellation.joinable())
        {
            cancellation.join();
        }
        CloseHandle(response_pipe);
        CloseHandle(cancellation_event);
        static_cast<void>(CoDisableCallCancellation(nullptr));
        return 0;
    }
}

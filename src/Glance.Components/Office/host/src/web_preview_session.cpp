#include "../include/web_preview_session.h"
#include "../include/presentation_package.h"
#include "../../../Common/preview_directory_cleanup.h"

#include <wrl.h>
#include <WebView2.h>
#include <winrt/base.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <wincrypt.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <stdexcept>
#include <vector>

namespace
{
    using Microsoft::WRL::Callback;
    using namespace std::chrono_literals;
    using Status = glance::contracts::native_preview::Status;
    constexpr wchar_t origin[] = L"https://office.glance.invalid/";
    constexpr wchar_t entry_uri[] = L"https://office.glance.invalid/index.html";
    constexpr UINT failure_message = WM_APP + 71;

    void trace(std::wstring_view message) noexcept
    {
#ifdef _DEBUG
        try
        {
            wchar_t path[32768]{};
            const DWORD length = GetEnvironmentVariableW(L"GLANCE_OFFICE_TRACE", path, static_cast<DWORD>(std::size(path)));
            if (length == 0 || length >= std::size(path)) return;
            const HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return;
            const auto line = std::to_wstring(GetTickCount64()) + L" " + std::wstring(message) + L"\r\n";
            DWORD written{};
            WriteFile(file, line.data(), static_cast<DWORD>(line.size() * sizeof(wchar_t)), &written, nullptr);
            CloseHandle(file);
        }
        catch (...) {}
#else
        static_cast<void>(message);
#endif
    }

    std::filesystem::path web_directory()
    {
        wchar_t path[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        if (length == 0 || length >= std::size(path)) return {};
        return std::filesystem::path(path).parent_path() / L"web";
    }

    void clean_exited_directories(const std::filesystem::path& root, bool require_browser_exit) noexcept
    {
        try
        {
            const DWORD root_attributes = GetFileAttributesW(root.c_str());
            if (root_attributes == INVALID_FILE_ATTRIBUTES ||
                (root_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return;
            std::error_code error;
            const auto deadline = GetTickCount64() + 100;
            for (const auto& entry : std::filesystem::directory_iterator(root, error))
            {
                if (GetTickCount64() >= deadline) break;
                const DWORD attributes = GetFileAttributesW(entry.path().c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
                    (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue;
                const auto name = entry.path().filename().wstring();
                const auto separator = name.find(L'-');
                if (separator == std::wstring::npos) continue;
                GUID id{};
                if (FAILED(CLSIDFromString(name.c_str() + separator + 1, &id)) ||
                    !glance::components::preview_owner_has_exited(std::wstring_view(name).substr(0, separator))) continue;
                if (require_browser_exit)
                {
                    std::wifstream owner(entry.path() / L"browser.pid");
                    std::wstring browser;
                    if (!(owner >> browser) || !glance::components::preview_owner_has_exited(browser)) continue;
                }
                std::filesystem::remove_all(entry.path(), error);
            }
        }
        catch (...) {}
    }

    bool record_browser_state(const std::filesystem::path& profile, ICoreWebView2Environment* environment) noexcept
    {
        try
        {
            winrt::com_ptr<ICoreWebView2Environment> owner;
            owner.copy_from(environment);
            winrt::com_ptr<ICoreWebView2ProcessInfoCollection> processes;
            winrt::check_hresult(owner.as<ICoreWebView2Environment8>()->GetProcessInfos(processes.put()));
            UINT count{};
            winrt::check_hresult(processes->get_Count(&count));
            bool browser_found = false;
            for (UINT index = 0; index < count; ++index)
            {
                winrt::com_ptr<ICoreWebView2ProcessInfo> process;
                winrt::check_hresult(processes->GetValueAtIndex(index, process.put()));
                COREWEBVIEW2_PROCESS_KIND kind{};
                winrt::check_hresult(process->get_Kind(&kind));
                if (kind != COREWEBVIEW2_PROCESS_KIND_BROWSER) continue;
                INT32 pid{};
                winrt::check_hresult(process->get_ProcessId(&pid));
                std::ofstream marker(profile / L"browser.pid");
                marker << pid;
                browser_found = true;
            }
            return !browser_found;
        }
        catch (...) { return false; }
    }

    std::vector<BYTE> read_document(const std::wstring& path, const std::atomic_bool& stopped)
    {
        const HANDLE raw = CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (raw == INVALID_HANDLE_VALUE) throw std::runtime_error("Document open failed");
        winrt::handle file(raw);
        LARGE_INTEGER size{};
        if (!file || file.get() == INVALID_HANDLE_VALUE || !GetFileSizeEx(file.get(), &size) ||
            size.QuadPart <= 0 || size.QuadPart > 128LL * 1024 * 1024) throw std::runtime_error("Invalid document size");
        std::vector<BYTE> bytes(static_cast<std::size_t>(size.QuadPart));
        std::size_t offset = 0;
        while (offset < bytes.size())
        {
            if (stopped.load()) throw std::runtime_error("Cancelled");
            DWORD read{};
            const DWORD count = static_cast<DWORD>(std::min<std::size_t>(256 * 1024, bytes.size() - offset));
            if (!ReadFile(file.get(), bytes.data() + offset, count, &read, nullptr) || read == 0)
                throw std::runtime_error("Document read failed");
            offset += read;
        }
        return bytes;
    }

    std::optional<bool> detect_legacy_format(const std::wstring& path, const std::atomic_bool& stopped)
    {
        if (stopped.load()) throw std::runtime_error("Cancelled");
        winrt::handle file(CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        BYTE header[8]{};
        DWORD read{};
        if (!file || file.get() == INVALID_HANDLE_VALUE ||
            !ReadFile(file.get(), header, sizeof(header), &read, nullptr) || read != sizeof(header))
            throw std::runtime_error("Document header read failed");
        if (stopped.load()) throw std::runtime_error("Cancelled");
        constexpr BYTE compound[] = { 0xd0, 0xcf, 0x11, 0xe0, 0xa1, 0xb1, 0x1a, 0xe1 };
        constexpr BYTE zip[] = { 0x50, 0x4b, 0x03, 0x04 };
        if (std::memcmp(header, compound, sizeof(compound)) == 0) return true;
        if (std::memcmp(header, zip, sizeof(zip)) == 0) return false;
        return std::nullopt;
    }

    std::wstring convert_image(const std::wstring& encoded)
    {
        DWORD length{};
        if (encoded.size() > 12 * 1024 * 1024 ||
            !CryptStringToBinaryW(encoded.c_str(), static_cast<DWORD>(encoded.size()), CRYPT_STRING_BASE64,
                nullptr, &length, nullptr, nullptr) || length == 0 || length > 8 * 1024 * 1024) return {};
        std::vector<BYTE> bytes(length);
        if (!CryptStringToBinaryW(encoded.c_str(), static_cast<DWORD>(encoded.size()), CRYPT_STRING_BASE64,
                bytes.data(), &length, nullptr, nullptr)) return {};
        winrt::com_ptr<IStream> stream;
        stream.attach(SHCreateMemStream(bytes.data(), length));
        if (!stream) return {};
        Gdiplus::Image source(stream.get());
        const auto width = source.GetWidth(), height = source.GetHeight();
        if (source.GetLastStatus() != Gdiplus::Ok || width == 0 || height == 0 ||
            static_cast<std::uint64_t>(width) * height > 64ULL * 1024 * 1024) return {};
        const double scale = std::min(2.0, 2048.0 / std::max(width, height));
        Gdiplus::Bitmap bitmap(std::max(1, static_cast<int>(width * scale)),
            std::max(1, static_cast<int>(height * scale)), PixelFormat32bppARGB);
        Gdiplus::Graphics graphics(&bitmap);
        graphics.Clear(Gdiplus::Color(0, 255, 255, 255));
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        if (graphics.DrawImage(&source, 0, 0, static_cast<int>(bitmap.GetWidth()),
                static_cast<int>(bitmap.GetHeight())) != Gdiplus::Ok) return {};
        UINT count{}, encoder_bytes{};
        Gdiplus::GetImageEncodersSize(&count, &encoder_bytes);
        std::vector<BYTE> storage(encoder_bytes);
        auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
        if (Gdiplus::GetImageEncoders(count, encoder_bytes, encoders) != Gdiplus::Ok) return {};
        CLSID png{};
        for (UINT index = 0; index < count; ++index)
            if (wcscmp(encoders[index].MimeType, L"image/png") == 0) png = encoders[index].Clsid;
        winrt::com_ptr<IStream> output;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, output.put())) ||
            bitmap.Save(output.get(), &png, nullptr) != Gdiplus::Ok) return {};
        STATSTG stat{};
        if (FAILED(output->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart > 32 * 1024 * 1024) return {};
        HGLOBAL memory{};
        if (FAILED(GetHGlobalFromStream(output.get(), &memory))) return {};
        auto* data = static_cast<BYTE*>(GlobalLock(memory));
        if (data == nullptr) return {};
        DWORD characters{};
        const DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
        std::wstring result;
        if (CryptBinaryToStringW(data, static_cast<DWORD>(stat.cbSize.QuadPart), flags, nullptr, &characters))
        {
            result.resize(characters);
            if (!CryptBinaryToStringW(data, static_cast<DWORD>(stat.cbSize.QuadPart), flags, result.data(), &characters)) result.clear();
            else result.resize(wcsnlen_s(result.data(), result.size()));
        }
        GlobalUnlock(memory);
        return result;
    }
}

namespace glance::office
{
    struct WebPreviewSession::State : std::enable_shared_from_this<State>
    {
        struct ImageTask { std::wstring id; std::future<std::wstring> result; std::wstring prefix{ L"image:" }; };
        struct MediaTask
        {
            winrt::com_ptr<ICoreWebView2WebResourceRequestedEventArgs> args;
            winrt::com_ptr<ICoreWebView2Deferral> deferral;
            std::future<PresentationMediaResponse> result;
        };
        HWND window{};
        std::filesystem::path profile;
        std::wstring path;
        bool format_ready{};
        glance::contracts::native_preview::ContentSize content_size{};
        bool presentation{};
        bool workbook{};
        std::atomic_bool stopped{};
        std::future<std::optional<bool>> detected_format;
        bool requested{}, sent{}, ready{}, failed{}, browser_exited{}, environment_requested{}, environment_completed{}, opening{ true };
        bool controller_pending{};
        ULONG_PTR gdiplus{};
        std::function<void()> failure;
        std::future<std::vector<BYTE>> source;
        std::vector<ImageTask> images;
        std::shared_ptr<PresentationPackage> presentation_package;
        std::vector<MediaTask> media_tasks;
        winrt::com_ptr<ICoreWebView2Environment> environment;
        winrt::com_ptr<ICoreWebView2Controller> controller;
        winrt::com_ptr<ICoreWebView2> view;
        glance::contracts::native_preview::PreviewVisuals visuals{};

        void error() noexcept
        {
            if (failed || stopped.load()) return;
            failed = true;
            if (ready && !opening && window) PostMessageW(window, failure_message, 0, 0);
        }

        void poll() noexcept
        {
            if (stopped.load()) return;
            try
            {
                if (!format_ready && detected_format.valid() && detected_format.wait_for(0ms) == std::future_status::ready)
                {
                    if (detected_format.get().value_or(false)) throw std::runtime_error("Legacy Office document requires system preview");
                    if (presentation) presentation_package = std::make_shared<PresentationPackage>(path);
                    source = std::async(std::launch::async, [this] {
                        std::error_code error;
                        if (presentation_package && std::filesystem::file_size(path, error) >= 64ULL * 1024 * 1024 && !error)
                            return presentation_package->project();
                        return read_document(path, stopped);
                    });
                    format_ready = true;
                }
                if (requested && !sent && source.valid() && source.wait_for(0ms) == std::future_status::ready)
                {
                    auto bytes = source.get();
                    auto modern_environment = environment.as<ICoreWebView2Environment12>();
                    auto modern_view = view.as<ICoreWebView2_17>();
                    winrt::com_ptr<ICoreWebView2SharedBuffer> buffer;
                    winrt::check_hresult(modern_environment->CreateSharedBuffer(bytes.size(), buffer.put()));
                    BYTE* data{};
                    winrt::check_hresult(buffer->get_Buffer(&data));
                    std::memcpy(data, bytes.data(), bytes.size());
                    // JSZip normalizes bytes in place; this buffer is a private copy of the source.
                    winrt::check_hresult(modern_view->PostSharedBufferToScript(buffer.get(),
                        COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_WRITE,
                        presentation ? L"{\"format\":\"pptx\"}" : workbook ? L"{\"format\":\"xlsx\"}" : L"{\"format\":\"docx\"}"));
                    buffer->Close();
                    sent = true;
                }
                for (auto iterator = images.begin(); iterator != images.end();)
                {
                    if (iterator->result.wait_for(0ms) != std::future_status::ready) { ++iterator; continue; }
                    const auto response = iterator->prefix + iterator->id + L":" + iterator->result.get();
                    winrt::check_hresult(view->PostWebMessageAsString(response.c_str()));
                    iterator = images.erase(iterator);
                }
                for (auto iterator = media_tasks.begin(); iterator != media_tasks.end();)
                {
                    if (iterator->result.wait_for(0ms) != std::future_status::ready) { ++iterator; continue; }
                    try
                    {
                        const auto response = iterator->result.get();
                        winrt::com_ptr<ICoreWebView2WebResourceResponse> native;
                        winrt::check_hresult(environment->CreateWebResourceResponse(response.stream.get(), response.status,
                            response.status == 206 ? L"Partial Content" : response.status == 200 ? L"OK" : L"Not Found",
                            response.headers.c_str(), native.put()));
                        iterator->args->put_Response(native.get());
                    }
                    catch (...) { respond_media(iterator->args.get(), 410); }
                    iterator->deferral->Complete();
                    iterator = media_tasks.erase(iterator);
                }
                if (sent && images.empty() && media_tasks.empty()) KillTimer(window, 1);
            }
            catch (...) { trace(L"poll failure " + std::to_wstring(winrt::to_hresult().value)); error(); }
        }

        void respond_media(ICoreWebView2WebResourceRequestedEventArgs* args, int status) noexcept
        {
            winrt::com_ptr<ICoreWebView2WebResourceResponse> response;
            if (environment && SUCCEEDED(environment->CreateWebResourceResponse(nullptr, status, L"Unavailable",
                L"Cache-Control: no-store\r\nContent-Length: 0", response.put()))) args->put_Response(response.get());
        }

        void request_media(ICoreWebView2WebResourceRequestedEventArgs* args) noexcept
        {
            LPWSTR uri{}, method{}, range{};
            try
            {
                if (stopped.load() || !sent || !presentation_package) { respond_media(args, 410); return; }
                if (media_tasks.size() >= 4) { respond_media(args, 503); return; }
                winrt::com_ptr<ICoreWebView2WebResourceRequest> request;
                winrt::check_hresult(args->get_Request(request.put()));
                winrt::check_hresult(request->get_Uri(&uri));
                winrt::check_hresult(request->get_Method(&method));
                const std::wstring_view address(uri), verb(method);
                constexpr std::wstring_view prefix = L"https://office-media.glance.invalid/";
                if (!address.starts_with(prefix) || (verb != L"GET" && verb != L"HEAD")) throw std::runtime_error("Invalid media request");
                const auto index_text = address.substr(prefix.size());
                if (index_text.empty() || index_text.size() > 4 || index_text.find_first_not_of(L"0123456789") != std::wstring_view::npos)
                    throw std::runtime_error("Invalid media index");
                const auto index = std::stoul(std::wstring(index_text));
                winrt::com_ptr<ICoreWebView2HttpRequestHeaders> headers;
                winrt::check_hresult(request->get_Headers(headers.put()));
                headers->GetHeader(L"Range", &range);
                MediaTask task;
                task.args.copy_from(args);
                winrt::check_hresult(args->GetDeferral(task.deferral.put()));
                task.result = std::async(std::launch::async,
                    [package = presentation_package, index, value = std::wstring(range ? range : L""), head = verb == L"HEAD"] {
                        return package->media(index, value, head);
                    });
                media_tasks.push_back(std::move(task));
                SetTimer(window, 1, 10, nullptr);
            }
            catch (...) { respond_media(args, 404); }
            CoTaskMemFree(uri); CoTaskMemFree(method); CoTaskMemFree(range);
        }

        void message(const std::wstring& text)
        {
            trace(text.starts_with(L"image:") ? L"image request" : text);
            if (text == L"source")
            {
                requested = true;
                view->PostWebMessageAsString(visuals.color_scheme == 1 ? L"theme:dark" : L"theme:light");
                SetTimer(window, 1, 10, nullptr);
                poll();
            }
            else if (text == L"ready") ready = true;
            else if (text.starts_with(L"size:"))
            {
                const auto separator = text.find(L':', 5);
                if (separator != std::wstring::npos && text.size() <= 32)
                {
                    const auto width_text = text.substr(5, separator - 5);
                    const auto height_text = text.substr(separator + 1);
                    if (!width_text.empty() && !height_text.empty() &&
                        width_text.find_first_not_of(L"0123456789") == std::wstring::npos &&
                        height_text.find_first_not_of(L"0123456789") == std::wstring::npos)
                    {
                        const auto width = wcstoul(width_text.c_str(), nullptr, 10);
                        const auto height = wcstoul(height_text.c_str(), nullptr, 10);
                        if (width > 0 && height > 0 && width <= 1000000 && height <= 1000000)
                            content_size = { width, height };
                    }
                }
            }
            else if (text == L"error" || text.starts_with(L"error:")) error();
            else if (text.starts_with(L"image:") && images.size() < 2)
            {
                const auto separator = text.find(L':', 6);
                if (separator == std::wstring::npos || separator > 16) { error(); return; }
                const auto id = text.substr(6, separator - 6);
                if (id.empty() || id.find_first_not_of(L"0123456789") != std::wstring::npos) { error(); return; }
                images.push_back({ id, std::async(std::launch::async, [encoded = text.substr(separator + 1)] {
                    try { return convert_image(encoded); } catch (...) { return std::wstring{}; }
                }) });
                SetTimer(window, 1, 10, nullptr);
            }
        }

        static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) noexcept
        {
            auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (message == WM_NCCREATE)
            {
                state = static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            }
            if (state && message == WM_TIMER) { state->poll(); return 0; }
            if (state && message == failure_message)
            {
                const auto keep_alive = state->shared_from_this();
                try { if (state->failure) state->failure(); } catch (...) {}
                return 0;
            }
            if (message == WM_NCDESTROY) SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
    };

    WebPreviewSession::~WebPreviewSession() { close(); }

    bool WebPreviewSession::available(const std::wstring& path) const
    {
        std::error_code error;
        const auto extension = std::filesystem::path(path).extension().wstring();
        const bool modern = _wcsicmp(extension.c_str(), L".docx") == 0 || _wcsicmp(extension.c_str(), L".pptx") == 0 ||
            _wcsicmp(extension.c_str(), L".xlsx") == 0;
        if (!std::filesystem::is_regular_file(web_directory() / L"index.html", error)) return false;
        if (modern) return true;
        const bool legacy_extension = _wcsicmp(extension.c_str(), L".doc") == 0 ||
            _wcsicmp(extension.c_str(), L".ppt") == 0 || _wcsicmp(extension.c_str(), L".xls") == 0;
        if (!legacy_extension) return false;
        try
        {
            // Some Office files have an Open XML payload despite their legacy extension.
            // This bounded header read runs only in the private host.
            const std::atomic_bool stopped{};
            const auto legacy = detect_legacy_format(path, stopped);
            return legacy.has_value() && !*legacy;
        }
        catch (...) { return false; }
    }

    bool WebPreviewSession::active() const noexcept { return state_ != nullptr; }

    glance::contracts::native_preview::ContentSize WebPreviewSession::content_size() const noexcept
    {
        return state_ ? state_->content_size : glance::contracts::native_preview::ContentSize{};
    }

    Status WebPreviewSession::open(const std::wstring& path, HWND parent, const RECT& bounds,
        const glance::contracts::native_preview::PreviewVisuals& visuals, HANDLE cancellation,
        std::function<void()> failure)
    {
        close();
        trace(L"open");
        const auto state = state_ = std::make_shared<State>();
        state->failure = std::move(failure);
        state->visuals = visuals;
        state->path = path;
        const auto extension = std::filesystem::path(path).extension().wstring();
        state->presentation = _wcsicmp(extension.c_str(), L".pptx") == 0 || _wcsicmp(extension.c_str(), L".ppt") == 0;
        state->workbook = _wcsicmp(extension.c_str(), L".xlsx") == 0 || _wcsicmp(extension.c_str(), L".xls") == 0;
        try
        {
            GUID id{};
            winrt::check_hresult(CoCreateGuid(&id));
            wchar_t guid[40]{};
            StringFromGUID2(id, guid, static_cast<int>(std::size(guid)));
            state->profile = std::filesystem::temp_directory_path() / L"Glance" / L"OfficeWebView" /
                (std::to_wstring(GetCurrentProcessId()) + L"-" + guid);
            std::filesystem::create_directories(state->profile);
            Gdiplus::GdiplusStartupInput graphics;
            graphics.SuppressExternalCodecs = TRUE;
            if (Gdiplus::GdiplusStartup(&state->gdiplus, &graphics, nullptr) != Gdiplus::Ok)
                throw std::runtime_error("Graphics initialization failed");
            WNDCLASSW window_class{};
            window_class.lpfnWndProc = State::procedure;
            window_class.hInstance = GetModuleHandleW(nullptr);
            window_class.lpszClassName = L"Glance.Office.WebPreview";
            RegisterClassW(&window_class);
            state->window = CreateWindowExW(WS_EX_NOACTIVATE, window_class.lpszClassName, L"",
                WS_CHILD | WS_VISIBLE, bounds.left, bounds.top, bounds.right - bounds.left,
                bounds.bottom - bounds.top, parent, nullptr, window_class.hInstance, state.get());
            if (!state->window) throw std::runtime_error("Preview window creation failed");
            state->detected_format = std::async(std::launch::async, [owner = state.get()] {
                return detect_legacy_format(owner->path, owner->stopped);
            });
            SetTimer(state->window, 1, 10, nullptr);
            const std::weak_ptr<State> weak = state;
            state->environment_requested = true;
            winrt::check_hresult(CreateCoreWebView2EnvironmentWithOptions(nullptr, state->profile.c_str(), nullptr,
                Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                    [weak](HRESULT status, ICoreWebView2Environment* environment) noexcept -> HRESULT {
                        trace(L"environment " + std::to_wstring(status));
                        const auto current = weak.lock();
                        if (!current) return S_OK;
                        current->environment_completed = true;
                        try
                        {
                            winrt::check_hresult(status);
                            if (!environment) throw std::runtime_error("Missing browser environment");
                            current->environment.copy_from(environment);
                            if (current->stopped.load())
                            {
                                current->browser_exited = record_browser_state(current->profile, environment);
                                return S_OK;
                            }
                            EventRegistrationToken exit_event{};
                            winrt::check_hresult(current->environment.as<ICoreWebView2Environment5>()->add_BrowserProcessExited(
                                Callback<ICoreWebView2BrowserProcessExitedEventHandler>(
                                    [weak](ICoreWebView2Environment*, ICoreWebView2BrowserProcessExitedEventArgs*) noexcept -> HRESULT {
                                        if (const auto owner = weak.lock()) owner->browser_exited = true;
                                        return S_OK;
                                    }).Get(), &exit_event));
                            current->controller_pending = true;
                            winrt::check_hresult(environment->CreateCoreWebView2Controller(current->window,
                                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                    [weak](HRESULT created, ICoreWebView2Controller* controller) noexcept -> HRESULT {
                                        trace(L"controller " + std::to_wstring(created));
                                        const auto target = weak.lock();
                                        if (!target) { if (controller) controller->Close(); return S_OK; }
                                        target->controller_pending = false;
                                        try
                                        {
                                            winrt::check_hresult(created);
                                            if (!controller) throw std::runtime_error("Missing browser controller");
                                            target->controller.copy_from(controller);
                                            winrt::check_hresult(controller->get_CoreWebView2(target->view.put()));
                                            UINT32 browser_pid{};
                                            winrt::check_hresult(target->view->get_BrowserProcessId(&browser_pid));
                                            std::ofstream browser_owner(target->profile / L"browser.pid");
                                            browser_owner << browser_pid;
                                            browser_owner.close();
                                            if (target->stopped.load())
                                            {
                                                controller->Close();
                                                target->view = nullptr;
                                                target->controller = nullptr;
                                                return S_OK;
                                            }
                                            RECT rectangle{};
                                            GetClientRect(target->window, &rectangle);
                                            controller->put_Bounds(rectangle);
                                            controller->put_IsVisible(TRUE);
                                            winrt::com_ptr<ICoreWebView2Settings> settings;
                                            target->view->get_Settings(settings.put());
                                            settings->put_AreDevToolsEnabled(FALSE);
                                            settings->put_AreDefaultScriptDialogsEnabled(FALSE);
                                            settings->put_IsStatusBarEnabled(FALSE);
                                            auto mapped = target->view.as<ICoreWebView2_3>();
                                            winrt::check_hresult(mapped->SetVirtualHostNameToFolderMapping(
                                                L"office.glance.invalid", web_directory().c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS));
                                            EventRegistrationToken event{};
                                            winrt::check_hresult(target->view->AddWebResourceRequestedFilter(
                                                L"https://office-media.glance.invalid/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL));
                                            winrt::check_hresult(target->view->add_WebResourceRequested(
                                                Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                                                    [weak](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) noexcept -> HRESULT {
                                                        if (const auto owner = weak.lock()) owner->request_media(args);
                                                        return S_OK;
                                                    }).Get(), &event));
                                            target->view->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                                [weak](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) noexcept -> HRESULT {
                                                    const auto owner = weak.lock();
                                                    if (!owner || owner->stopped.load()) return S_OK;
                                                    LPWSTR source{}, text{};
                                                    try
                                                    {
                                                        winrt::check_hresult(args->get_Source(&source));
                                                        if (std::wstring_view(source).starts_with(origin) && SUCCEEDED(args->TryGetWebMessageAsString(&text))) owner->message(text);
                                                    }
                                                    catch (...) { owner->error(); }
                                                    CoTaskMemFree(source); CoTaskMemFree(text);
                                                    return S_OK;
                                                }).Get(), &event);
                                            target->view->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
                                                [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) noexcept -> HRESULT {
                                                    LPWSTR uri{};
                                                    if (SUCCEEDED(args->get_Uri(&uri)))
                                                    {
                                                        const std::wstring_view value(uri);
                                                        constexpr std::wstring_view entry{ entry_uri };
                                                        if (value != entry && !(value.starts_with(entry) && value.size() > entry.size() && value[entry.size()] == L'#')) args->put_Cancel(TRUE);
                                                    }
                                                    CoTaskMemFree(uri);
                                                    return S_OK;
                                                }).Get(), &event);
                                            target->view->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>(
                                                [weak](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) noexcept -> HRESULT {
                                                    COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
                                                    args->get_ProcessFailedKind(&kind);
                                                    trace(L"WebView process failed " + std::to_wstring(kind));
                                                    if (const auto owner = weak.lock()) owner->error();
                                                    return S_OK;
                                                }).Get(), &event);
                                            winrt::check_hresult(target->view->Navigate(entry_uri));
                                        }
                                        catch (...) { target->error(); }
                                        return S_OK;
                                    }).Get()));
                        }
                        catch (...) { current->controller_pending = false; current->error(); }
                        return S_OK;
                    }).Get()));
            const auto deadline = GetTickCount64() + 8000;
            while (!state->ready && !state->failed)
            {
                if (WaitForSingleObject(cancellation, 0) == WAIT_OBJECT_0) { close(); return Status::cancelled; }
                if (GetTickCount64() >= deadline) break;
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                {
                    if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); close(); return Status::cancelled; }
                    TranslateMessage(&message); DispatchMessageW(&message);
                }
                state->poll();
                MsgWaitForMultipleObjects(1, &cancellation, FALSE, 10, QS_ALLINPUT);
            }
            if (state->ready && !state->failed) { state->opening = false; return Status::success; }
        }
        catch (...) {}
        close();
        return Status::preview_failed;
    }

    void WebPreviewSession::resize(const RECT& bounds)
    {
        if (!state_) return;
        SetWindowPos(state_->window, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
            bounds.bottom - bounds.top, SWP_NOACTIVATE | SWP_NOZORDER);
        RECT rectangle{}; GetClientRect(state_->window, &rectangle);
        if (state_->controller) state_->controller->put_Bounds(rectangle);
    }

    void WebPreviewSession::set_visuals(const glance::contracts::native_preview::PreviewVisuals& visuals)
    {
        if (!state_) return;
        state_->visuals = visuals;
        if (state_->view) state_->view->PostWebMessageAsString(visuals.color_scheme == 1 ? L"theme:dark" : L"theme:light");
    }

    void WebPreviewSession::close() noexcept
    {
        auto state = std::move(state_);
        if (!state) return;
        state->stopped.store(true);
        if (state->presentation_package) state->presentation_package->cancel();
        if (state->window) KillTimer(state->window, 1);
        if (state->controller) state->controller->Close();
        state->view = nullptr; state->controller = nullptr;
        if (state->window) DestroyWindow(state->window);
        if (state->detected_format.valid()) state->detected_format.wait();
        if (state->source.valid()) state->source.wait();
        for (auto& task : state->media_tasks)
        {
            task.result.wait();
            task.deferral->Complete();
        }
        state->media_tasks.clear();
        for (auto& image : state->images) image.result.wait();
        state->images.clear();
        if (state->gdiplus) Gdiplus::GdiplusShutdown(state->gdiplus);
        // The environment must remain alive to deliver the browser exit notification.
        const auto deadline = GetTickCount64() + 1000;
        bool quitting = false;
        while (((state->environment_requested && !state->environment_completed) ||
                (state->environment && !state->browser_exited)) && !quitting && GetTickCount64() < deadline)
        {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                if (message.message == WM_QUIT)
                {
                    PostQuitMessage(static_cast<int>(message.wParam));
                    quitting = true;
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (!state->browser_exited) MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        }
        if (state->environment && !state->browser_exited)
        {
            const bool browser_absent = record_browser_state(state->profile, state->environment.get());
            state->browser_exited = browser_absent && !state->controller_pending;
        }
        state->environment = nullptr;
        std::error_code error;
        if (!state->profile.empty())
        {
            // Keep the owner marker intact if browser shutdown is still in progress.
            if (state->browser_exited || !state->environment_requested) std::filesystem::remove_all(state->profile, error);
            clean_exited_directories(state->profile.parent_path(), true);
        }
    }
}

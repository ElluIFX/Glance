#include "access_client.h"
#include "glance/contracts/diagnostics.h"
#include "../../version.h"
#include <new>

namespace glance::core
{
    namespace access = contracts::access;
    AccessClient::AccessClient(HWND window) : window_(window)
    {
        mapping_.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(access::SharedState), nullptr));
        if (mapping_) state_ = static_cast<access::SharedState*>(MapViewOfFile(mapping_.get(), FILE_MAP_READ | FILE_MAP_WRITE,
            0, 0, sizeof(access::SharedState)));
        if (state_) { new (state_) access::SharedState{}; state_->input.enabled.store(false); }
        worker_ = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested())
            {
                try { ensure_connected(); } catch (...) { std::lock_guard lock(mutex_); disconnect(); }
                Sleep(100);
            }
        });
    }

    AccessClient::~AccessClient()
    {
        worker_.request_stop();
        worker_.join();
        disconnect();
        if (state_) UnmapViewOfFile(state_);
    }

    void AccessClient::disconnect() noexcept
    {
        connected_.store(false, std::memory_order_release);
        if (state_) state_->input.enabled.store(false, std::memory_order_release);
        pipe_.reset(); lease_.reset();
        retry_at_ = GetTickCount64() + 2000;
    }

    bool AccessClient::ready() const noexcept
    {
        return state_ && connected_.load(std::memory_order_acquire) &&
            GetTickCount64() - state_->host_tick.load(std::memory_order_acquire) < 500;
    }

    void AccessClient::update_input(const access::InputState& source) noexcept
    {
        if (!state_) return;
        auto& target = state_->input;
        target.ui_connected.store(source.ui_connected.load(std::memory_order_acquire));
        target.eligible_selection.store(source.eligible_selection.load(std::memory_order_acquire));
        target.preview_active.store(source.preview_active.load(std::memory_order_acquire));
        target.text_input_active.store(source.text_input_active.load(std::memory_order_acquire));
        target.valid_until.store(GetTickCount64() + 500, std::memory_order_release);
        target.enabled.store(ready(), std::memory_order_release);
    }

    std::uint64_t AccessClient::hook_events() const noexcept
    {
        return state_ ? state_->hook_events.load(std::memory_order_relaxed) : 0;
    }

    void AccessClient::ensure_connected()
    {
        std::lock_guard lock(mutex_);
        if (connected_.load() && !ready()) disconnect();
        if (connected_.load())
        {
            if (refresh_requested_.exchange(false))
            {
                access::Header header{}; header.operation = access::Operation::refresh_hook;
                if (!access::transfer(pipe_.get(), &header, sizeof(header), true, 500) ||
                    !access::transfer(pipe_.get(), &header, sizeof(header), false, 500) ||
                    header.signature != access::magic || header.size || header.operation != access::Operation::refresh_hook)
                    disconnect();
            }
            return;
        }
        if (!state_ || GetTickCount64() < retry_at_ || access::installation_mode() != access::InstallationMode::installed) return;
        retry_at_ = GetTickCount64() + 2000;
        const auto service_error = access::start_service();
        if (service_error != ERROR_SUCCESS)
        {
            contracts::log_event(L"Access service startup failed: " + std::to_wstring(service_error));
            return;
        }
        const auto deadline = GetTickCount64() + 5000;
        do
        {
            lease_.reset(CreateFileW(access::service_pipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
            if (lease_) break;
            if (GetLastError() != ERROR_PIPE_BUSY && GetLastError() != ERROR_FILE_NOT_FOUND)
            {
                contracts::log_event(L"Access service connection failed: " + std::to_wstring(GetLastError()));
                return;
            }
            Sleep(50);
        } while (GetTickCount64() < deadline);
        if (!lease_) return;
        ULONG server{};
        unique_handle process;
        if (!GetNamedPipeServerProcessId(lease_.get(), &server) || !access::service_process(server))
        { contracts::log_event(L"Access service identity verification failed: " + std::to_wstring(server) + L" / " + std::to_wstring(GetLastError())); disconnect(); return; }
        access::BootstrapRequest request{};
        wcscpy_s(request.app_version, GLANCE_VERSION_WSTRING);
        request.mapping = reinterpret_cast<UINT_PTR>(mapping_.get());
        request.notification_window = reinterpret_cast<UINT_PTR>(window_);
        request.notification_message = WM_APP + 1;
        state_->host_tick.store(0);
        access::BootstrapReply reply{};
        if (!access::transfer(lease_.get(), &request, sizeof(request), true) ||
            !access::transfer(lease_.get(), &reply, sizeof(reply), false, 5000))
        {
            contracts::log_event(L"Access bootstrap transport failed: " + std::to_wstring(GetLastError()));
            disconnect(); return;
        }
        if (reply.error ||
            !reply.host_pid || wcsnlen_s(reply.endpoint, ARRAYSIZE(reply.endpoint)) == ARRAYSIZE(reply.endpoint) ||
            !std::wstring_view(reply.endpoint).starts_with(L"\\\\.\\pipe\\Glance.AccessHost."))
        {
            contracts::log_event(L"Access bootstrap rejected: " + std::to_wstring(reply.error) + L" / " +
                std::wstring(reply.endpoint, wcsnlen_s(reply.endpoint, ARRAYSIZE(reply.endpoint))));
            disconnect(); return;
        }
        do
        {
            pipe_.reset(CreateFileW(reply.endpoint, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
            if (pipe_) break;
            Sleep(25);
        } while (GetTickCount64() < deadline);
        if (!pipe_ || !GetNamedPipeServerProcessId(pipe_.get(), &server) || server != reply.host_pid)
        { contracts::log_event(L"Access helper connection failed: " + std::to_wstring(GetLastError())); disconnect(); return; }
        process.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, server));
        if (!process || !access::process_image(process.get(), access::executable_directory() / L"Glance.AccessHost.exe"))
        { disconnect(); return; }
        connected_.store(true, std::memory_order_release);
        contracts::log_event(L"Privileged access helper connected");
    }

    std::string AccessClient::exchange(access::Operation operation, std::string payload)
    {
        std::lock_guard lock(mutex_);
        if (!ready()) throw std::runtime_error("Privileged access unavailable");
        access::Header header{}; header.operation = operation; header.size = static_cast<std::uint32_t>(payload.size());
        if (!access::transfer(pipe_.get(), &header, sizeof(header), true, 750) ||
            !access::transfer(pipe_.get(), payload.data(), header.size, true, 750) ||
            !access::transfer(pipe_.get(), &header, sizeof(header), false, 750) ||
            header.signature != access::magic || header.operation != operation || header.flags || header.size > access::maximum_payload)
        { disconnect(); throw std::runtime_error("Privileged access request failed"); }
        payload.resize(header.size);
        if (!access::transfer(pipe_.get(), payload.data(), header.size, false, 750))
        { disconnect(); throw std::runtime_error("Privileged access response failed"); }
        return payload;
    }

    contracts::SelectionSnapshot AccessClient::selection(bool& suppress)
    {
        return access_wire::selection(exchange(access::Operation::selection, {}), suppress);
    }

    GalleryResponse AccessClient::gallery(const GalleryCommand& command)
    {
        auto response = access_wire::gallery_response(exchange(access::Operation::gallery, access_wire::gallery(command)));
        if (response.window_id != command.window_id || response.request_id != command.request_id || response.operation != command.operation)
            throw std::runtime_error("Privileged gallery response mismatch");
        return response;
    }

    bool AccessClient::requires_access(HWND window) noexcept
    {
        DWORD pid{};
        GetWindowThreadProcessId(window, &pid);
        if (!pid || pid == GetCurrentProcessId()) return false;
        unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        HANDLE raw{};
        if (!process || !OpenProcessToken(process.get(), TOKEN_QUERY, &raw)) return true;
        unique_handle token(raw);
        TOKEN_ELEVATION elevation{};
        DWORD size{};
        return !GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size) || elevation.TokenIsElevated != 0;
    }
}

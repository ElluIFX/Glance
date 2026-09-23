#include "pch.h"
#include "command_server.h"

namespace glance::app
{
    CommandServer::CommandServer(Handler handler, Replied replied)
        : handler_(std::move(handler)), replied_(std::move(replied)), stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
    {
        if (!stop_.value) throw std::runtime_error("Cannot create CLI cancellation event");
        try { for (int i = 0; i < 4; ++i) workers_.emplace_back([this] { run(); }); }
        catch (...) { stop(); throw; }
    }
    CommandServer::~CommandServer() { stop(); }
    void CommandServer::stop() noexcept
    {
        SetEvent(stop_.value);
        for (auto& worker : workers_) if (worker.joinable()) CancelSynchronousIo(worker.native_handle());
        for (auto& worker : workers_) if (worker.joinable()) worker.join();
    }
    void CommandServer::run() noexcept
    {
        using namespace glance::cli;
        try
        {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
            const auto name = pipe_name();
            const auto descriptor = L"D:P(A;;GA;;;" + user_sid() + L")";
            PSECURITY_DESCRIPTOR security{};
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor.c_str(), SDDL_REVISION_1, &security, nullptr)) return;
            const std::unique_ptr<void, decltype(&LocalFree)> security_owner(security, LocalFree);
            SECURITY_ATTRIBUTES attributes{ sizeof(attributes), security, FALSE };
            while (WaitForSingleObject(stop_.value, 0) != WAIT_OBJECT_0)
            {
                Handle pipe(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                    4, 65536, 65536, 0, &attributes));
                if (pipe.value == INVALID_HANDLE_VALUE) return;
                Handle connected(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                OVERLAPPED operation{};
                operation.hEvent = connected.value;
                const auto success = ConnectNamedPipe(pipe.value, &operation);
                const auto error = success ? ERROR_SUCCESS : GetLastError();
                if (error == ERROR_IO_PENDING)
                {
                    HANDLE events[]{ connected.value, stop_.value };
                    const auto wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
                    DWORD ignored{};
                    if (wait != WAIT_OBJECT_0) CancelIoEx(pipe.value, &operation);
                    const auto completed = GetOverlappedResult(pipe.value, &operation, &ignored, TRUE);
                    if (wait != WAIT_OBJECT_0) return;
                    if (!completed) continue;
                }
                else if (error != ERROR_SUCCESS && error != ERROR_PIPE_CONNECTED) continue;
                Header request;
                try
                {
                    ULONG client_session{};
                    DWORD own_session{};
                    if (!GetNamedPipeClientSessionId(pipe.value, &client_session) ||
                        !ProcessIdToSessionId(GetCurrentProcessId(), &own_session) || client_session != own_session)
                        throw Error(7, "session_mismatch", "Wrong login session");
                    auto payload = receive(pipe.value, request, GetTickCount64() + 10000, stop_.value);
                    auto response = handler_(std::move(payload), stop_.value, pipe.value);
                    if (response.size() > maximum_payload) throw Error(1, "response_too_large", "Response exceeds protocol limit");
                    if (send(pipe.value, response, request.request, GetTickCount64() + 5000, stop_.value))
                    {
                        unsigned char ack{};
                        transfer(pipe.value, &ack, 1, false, GetTickCount64() + 1000, stop_.value);
                    }
                    replied_(response);
                }
                catch (const Error& error)
                {
                    using namespace winrt::Windows::Data::Json;
                    JsonObject result, detail;
                    result.SetNamedValue(L"schema_version", JsonValue::CreateNumberValue(1));
                    result.SetNamedValue(L"ok", JsonValue::CreateBooleanValue(false));
                    result.SetNamedValue(L"command", JsonValue::CreateStringValue(L""));
                    result.SetNamedValue(L"data", JsonValue::CreateNullValue());
                    detail.SetNamedValue(L"code", JsonValue::CreateNumberValue(error.code));
                    detail.SetNamedValue(L"name", JsonValue::CreateStringValue(winrt::to_hstring(error.name)));
                    detail.SetNamedValue(L"message", JsonValue::CreateStringValue(winrt::to_hstring(error.what())));
                    result.SetNamedValue(L"error", detail);
                    if (send(pipe.value, winrt::to_string(result.Stringify()), request.request, GetTickCount64() + 1000, stop_.value))
                    {
                        unsigned char ack{};
                        transfer(pipe.value, &ack, 1, false, GetTickCount64() + 1000, stop_.value);
                    }
                }
                catch (...) { /* Malformed/disconnected clients cannot affect the UI. */ }
                DisconnectNamedPipe(pipe.value);
            }
        }
        catch (...) { /* Server lifetime is bounded by the owning application. */ }
    }
}

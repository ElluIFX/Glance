#include "pipe_server.h"
#include "unique_handle.h"

#include <sddl.h>

#include <filesystem>
#include <string>
#include <vector>

namespace
{
    std::vector<std::byte> token_user(HANDLE process)
    {
        HANDLE raw_token{};
        if (!OpenProcessToken(process, TOKEN_QUERY, &raw_token))
        {
            return {};
        }
        glance::core::unique_handle token(raw_token);
        DWORD required{};
        GetTokenInformation(token.get(), TokenUser, nullptr, 0, &required);
        std::vector<std::byte> buffer(required);
        if (required == 0 || !GetTokenInformation(token.get(), TokenUser, buffer.data(), required, &required))
        {
            return {};
        }
        return buffer;
    }

    std::wstring current_user_sid()
    {
        auto user = token_user(GetCurrentProcess());
        if (user.empty())
        {
            return {};
        }
        const auto* token = reinterpret_cast<const TOKEN_USER*>(user.data());
        PWSTR raw_sid{};
        if (!ConvertSidToStringSidW(token->User.Sid, &raw_sid))
        {
            return {};
        }
        std::wstring sid = raw_sid;
        LocalFree(raw_sid);
        return sid;
    }

    std::filesystem::path expected_client_path()
    {
        std::wstring module(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
        module.resize(length);
        return std::filesystem::path(module).parent_path() / L"Glance.exe";
    }

    bool authorize_client(HANDLE pipe)
    {
        ULONG client_process_id{};
        if (!GetNamedPipeClientProcessId(pipe, &client_process_id))
        {
            return false;
        }
        DWORD server_session{};
        DWORD client_session{};
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &server_session) ||
            !ProcessIdToSessionId(client_process_id, &client_session) ||
            server_session != client_session)
        {
            return false;
        }

        glance::core::unique_handle client(OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            client_process_id));
        if (!client)
        {
            return false;
        }
        const auto server_user = token_user(GetCurrentProcess());
        const auto client_user = token_user(client.get());
        if (server_user.empty() || client_user.empty())
        {
            return false;
        }
        const auto* server_token = reinterpret_cast<const TOKEN_USER*>(server_user.data());
        const auto* client_token = reinterpret_cast<const TOKEN_USER*>(client_user.data());
        if (!EqualSid(server_token->User.Sid, client_token->User.Sid))
        {
            return false;
        }

        std::wstring client_path(32768, L'\0');
        DWORD client_path_length = static_cast<DWORD>(client_path.size());
        if (!QueryFullProcessImageNameW(client.get(), 0, client_path.data(), &client_path_length))
        {
            return false;
        }
        client_path.resize(client_path_length);
        const auto expected = expected_client_path().wstring();
        return CompareStringOrdinal(
                   client_path.c_str(),
                   static_cast<int>(client_path.size()),
                   expected.c_str(),
                   static_cast<int>(expected.size()),
                   TRUE) == CSTR_EQUAL;
    }
}

namespace glance::core
{
    PipeServer::PipeServer(MessageHandler message_handler, ConnectionHandler connection_handler)
        : message_handler_(std::move(message_handler)), connection_handler_(std::move(connection_handler))
    {
    }

    PipeServer::~PipeServer()
    {
        stop();
    }

    bool PipeServer::start()
    {
        if (thread_.joinable())
        {
            return true;
        }
        stopping_.store(false, std::memory_order_release);
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void PipeServer::stop() noexcept
    {
        stopping_.store(true, std::memory_order_release);
        const HANDLE pipe = pipe_.exchange(nullptr, std::memory_order_acq_rel);
        if (pipe != nullptr && pipe != INVALID_HANDLE_VALUE)
        {
            CancelIoEx(pipe, nullptr);
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
        }
        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    bool PipeServer::send(
        glance::contracts::MessageType type,
        std::string_view payload,
        std::uint32_t flags)
    {
        if (payload.size() > glance::contracts::maximum_payload_size)
        {
            return false;
        }

        std::scoped_lock lock(write_mutex_);
        const HANDLE pipe = pipe_.load(std::memory_order_acquire);
        if (!connected_.load(std::memory_order_acquire) || pipe == nullptr || pipe == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        glance::contracts::FrameHeader header;
        header.type = type;
        header.flags = flags;
        header.payload_size = static_cast<std::uint32_t>(payload.size());
        header.correlation_id = correlation_id_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!write_exact(pipe, &header, sizeof(header)))
        {
            return false;
        }
        return payload.empty() || write_exact(pipe, payload.data(), static_cast<DWORD>(payload.size()));
    }

    bool PipeServer::read_exact(HANDLE pipe, void* buffer, DWORD size) noexcept
    {
        auto* output = static_cast<std::byte*>(buffer);
        DWORD total{};
        while (total < size && !stopping_.load(std::memory_order_acquire))
        {
            OVERLAPPED operation{};
            operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (operation.hEvent == nullptr)
            {
                return false;
            }
            DWORD read{};
            const BOOL started = ReadFile(pipe, output + total, size - total, &read, &operation);
            if (!started && GetLastError() == ERROR_IO_PENDING)
            {
                while (!stopping_.load(std::memory_order_acquire) &&
                       WaitForSingleObject(operation.hEvent, 100) == WAIT_TIMEOUT)
                {
                }
                if (stopping_.load(std::memory_order_acquire))
                {
                    CancelIoEx(pipe, &operation);
                    WaitForSingleObject(operation.hEvent, INFINITE);
                }
                else if (!GetOverlappedResult(pipe, &operation, &read, FALSE))
                {
                    read = 0;
                }
            }
            else if (!started)
            {
                read = 0;
            }
            CloseHandle(operation.hEvent);
            if (read == 0)
            {
                return false;
            }
            total += read;
        }
        return total == size;
    }

    bool PipeServer::write_exact(HANDLE pipe, const void* buffer, DWORD size) noexcept
    {
        const auto* input = static_cast<const std::byte*>(buffer);
        DWORD total{};
        while (total < size && !stopping_.load(std::memory_order_acquire))
        {
            OVERLAPPED operation{};
            operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (operation.hEvent == nullptr)
            {
                return false;
            }
            DWORD written{};
            const BOOL started = WriteFile(pipe, input + total, size - total, &written, &operation);
            if (!started && GetLastError() == ERROR_IO_PENDING)
            {
                const DWORD wait_result = WaitForSingleObject(operation.hEvent, 2000);
                if (wait_result == WAIT_OBJECT_0)
                {
                    if (!GetOverlappedResult(pipe, &operation, &written, FALSE))
                    {
                        written = 0;
                    }
                }
                else
                {
                    CancelIoEx(pipe, &operation);
                    WaitForSingleObject(operation.hEvent, INFINITE);
                    written = 0;
                }
            }
            else if (!started)
            {
                written = 0;
            }
            CloseHandle(operation.hEvent);
            if (written == 0)
            {
                return false;
            }
            total += written;
        }
        return total == size;
    }

    void PipeServer::run() noexcept
    {
        const auto user_sid = current_user_sid();
        if (user_sid.empty())
        {
            return;
        }
        const std::wstring security_definition =
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;" + user_sid + L")";
        PSECURITY_DESCRIPTOR descriptor{};
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                security_definition.c_str(),
                SDDL_REVISION_1,
                &descriptor,
                nullptr))
        {
            return;
        }
        SECURITY_ATTRIBUTES security{ sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE };

        while (!stopping_.load(std::memory_order_acquire))
        {
            HANDLE pipe = CreateNamedPipeW(
                glance::contracts::pipe_name,
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                1,
                glance::contracts::maximum_payload_size,
                glance::contracts::maximum_payload_size,
                0,
                &security);
            if (pipe == INVALID_HANDLE_VALUE)
            {
                break;
            }
            pipe_.store(pipe, std::memory_order_release);

            OVERLAPPED connection{};
            connection.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            BOOL connected{};
            if (connection.hEvent != nullptr)
            {
                connected = ConnectNamedPipe(pipe, &connection);
                if (!connected)
                {
                    const DWORD error = GetLastError();
                    if (error == ERROR_PIPE_CONNECTED)
                    {
                        connected = TRUE;
                    }
                    else if (error == ERROR_IO_PENDING)
                    {
                        while (!stopping_.load(std::memory_order_acquire) &&
                               WaitForSingleObject(connection.hEvent, 100) == WAIT_TIMEOUT)
                        {
                        }
                        if (stopping_.load(std::memory_order_acquire))
                        {
                            CancelIoEx(pipe, &connection);
                            WaitForSingleObject(connection.hEvent, INFINITE);
                        }
                        else
                        {
                            DWORD transferred{};
                            connected = GetOverlappedResult(pipe, &connection, &transferred, FALSE);
                        }
                    }
                }
                CloseHandle(connection.hEvent);
            }
            if (!connected || stopping_.load(std::memory_order_acquire) || !authorize_client(pipe))
            {
                if (pipe_.exchange(nullptr, std::memory_order_acq_rel) == pipe)
                {
                    CloseHandle(pipe);
                }
                continue;
            }

            connected_.store(true, std::memory_order_release);
            connection_handler_(true);

            while (!stopping_.load(std::memory_order_acquire))
            {
                glance::contracts::FrameHeader header;
                if (!read_exact(pipe, &header, sizeof(header)) || !glance::contracts::valid_header(header))
                {
                    break;
                }
                std::string payload(header.payload_size, '\0');
                if (header.payload_size > 0 && !read_exact(pipe, payload.data(), header.payload_size))
                {
                    break;
                }
                message_handler_(header.type, header.flags, payload);
            }

            connected_.store(false, std::memory_order_release);
            connection_handler_(false);
            if (pipe_.exchange(nullptr, std::memory_order_acq_rel) == pipe)
            {
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
            }
        }

        if (descriptor != nullptr)
        {
            LocalFree(descriptor);
        }
    }
}

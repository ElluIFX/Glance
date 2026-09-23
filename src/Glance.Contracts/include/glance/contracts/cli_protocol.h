#pragma once

#include <windows.h>
#include <sddl.h>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace glance::cli
{
    constexpr std::uint32_t magic = 0x494C4347;
    constexpr std::uint32_t version = 1;
    constexpr std::uint32_t maximum_payload = 1024 * 1024;
    struct Header
    {
        std::uint32_t signature{ magic };
        std::uint32_t protocol{ version };
        std::uint32_t size{};
        std::uint32_t reserved{};
        std::uint64_t request{};
    };
    static_assert(sizeof(Header) == 24);
    struct Error : std::runtime_error
    {
        int code;
        std::string name;
        Error(int value, std::string identifier, std::string message)
            : std::runtime_error(std::move(message)), code(value), name(std::move(identifier)) {}
    };
    struct Handle
    {
        HANDLE value{ INVALID_HANDLE_VALUE };
        explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : value(handle) {}
        ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
    };
    inline std::wstring executable_path()
    {
        std::wstring path(32768, L'\0');
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length == path.size()) throw Error(1, "executable_path", "Cannot resolve executable path");
        path.resize(length);
        return path;
    }
    inline std::wstring user_sid()
    {
        HANDLE raw{};
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw))
            throw Error(7, "token_access", "Cannot read user token");
        Handle token(raw);
        DWORD size{};
        GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
        std::vector<unsigned char> bytes(size);
        if (!GetTokenInformation(token.value, TokenUser, bytes.data(), size, &size))
            throw Error(7, "token_access", "Cannot read user identity");
        LPWSTR text{};
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &text))
            throw Error(7, "token_access", "Cannot format user identity");
        std::wstring sid(text);
        LocalFree(text);
        return sid;
    }
    inline std::wstring endpoint_suffix()
    {
        DWORD session{};
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &session))
            throw Error(1, "session", "Cannot determine login session");
        return user_sid() + L"." + std::to_wstring(session);
    }
    inline std::wstring pipe_name() { return LR"(\\.\pipe\Glance.CLI.v1.)" + endpoint_suffix(); }

    // Every operation has a deadline and cancellation event, including idle clients.
    inline bool transfer(HANDLE pipe, void* buffer, DWORD bytes, bool write,
        ULONGLONG deadline, HANDLE cancel)
    {
        auto cursor = static_cast<unsigned char*>(buffer);
        while (bytes)
        {
            Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!event.value) return false;
            OVERLAPPED operation{};
            operation.hEvent = event.value;
            DWORD done{};
            BOOL success = write ? WriteFile(pipe, cursor, bytes, &done, &operation)
                                 : ReadFile(pipe, cursor, bytes, &done, &operation);
            if (!success && GetLastError() != ERROR_IO_PENDING) return false;
            if (!success)
            {
                const auto now = GetTickCount64();
                HANDLE events[]{ event.value, cancel };
                const auto wait = WaitForMultipleObjects(cancel ? 2 : 1, events, FALSE,
                    now >= deadline ? 0 : static_cast<DWORD>((std::min)(deadline - now, 0xfffffffeULL)));
                if (wait != WAIT_OBJECT_0)
                {
                    CancelIoEx(pipe, &operation);
                    GetOverlappedResult(pipe, &operation, &done, TRUE);
                    return false;
                }
                if (!GetOverlappedResult(pipe, &operation, &done, FALSE)) return false;
            }
            if (!done) return false;
            cursor += done;
            bytes -= done;
        }
        return true;
    }
    inline std::string receive(HANDLE pipe, Header& header, ULONGLONG deadline, HANDLE cancel)
    {
        if (!transfer(pipe, &header, sizeof(header), false, deadline, cancel))
            throw Error(5, "transport_timeout", "Connection closed or timed out");
        if (header.signature != magic || header.protocol != version)
            throw Error(6, "protocol_mismatch", "Unsupported CLI protocol");
        if (header.reserved || !header.size || header.size > maximum_payload)
            throw Error(2, "invalid_frame", "Invalid request size or header");
        std::string payload(header.size, '\0');
        if (!transfer(pipe, payload.data(), header.size, false, deadline, cancel))
            throw Error(5, "transport_timeout", "Connection closed or timed out");
        return payload;
    }
    inline bool send(HANDLE pipe, std::string payload, std::uint64_t request, ULONGLONG deadline, HANDLE cancel)
    {
        if (payload.empty() || payload.size() > maximum_payload) return false;
        Header header;
        header.request = request;
        header.size = static_cast<std::uint32_t>(payload.size());
        return transfer(pipe, &header, sizeof(header), true, deadline, cancel) &&
            transfer(pipe, payload.data(), header.size, true, deadline, cancel);
    }
}

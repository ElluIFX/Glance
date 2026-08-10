#include "pch.h"
#include "download_service.h"
#include "../../version.h"

#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <optional>
#include <vector>

namespace
{
    constexpr DWORD download_buffer_bytes = 256 * 1024;

    class InternetHandle
    {
    public:
        explicit InternetHandle(HINTERNET value = nullptr) noexcept : value_(value)
        {
        }

        ~InternetHandle()
        {
            if (value_ != nullptr)
            {
                WinHttpCloseHandle(value_);
            }
        }

        InternetHandle(const InternetHandle&) = delete;
        InternetHandle& operator=(const InternetHandle&) = delete;

        [[nodiscard]] HINTERNET get() const noexcept
        {
            return value_;
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return value_ != nullptr;
        }

    private:
        HINTERNET value_{};
    };

    class FileHandle
    {
    public:
        explicit FileHandle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value)
        {
        }

        ~FileHandle()
        {
            close();
        }

        FileHandle(const FileHandle&) = delete;
        FileHandle& operator=(const FileHandle&) = delete;

        void close() noexcept
        {
            if (value_ != INVALID_HANDLE_VALUE)
            {
                CloseHandle(value_);
                value_ = INVALID_HANDLE_VALUE;
            }
        }

        [[nodiscard]] HANDLE get() const noexcept
        {
            return value_;
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return value_ != INVALID_HANDLE_VALUE;
        }

    private:
        HANDLE value_{ INVALID_HANDLE_VALUE };
    };

    class Sha256Hash
    {
    public:
        Sha256Hash()
        {
            if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
                    &algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
            {
                return;
            }
            DWORD object_size{};
            DWORD result_size{};
            if (!BCRYPT_SUCCESS(BCryptGetProperty(
                    algorithm_,
                    BCRYPT_OBJECT_LENGTH,
                    reinterpret_cast<PUCHAR>(&object_size),
                    sizeof(object_size),
                    &result_size,
                    0)))
            {
                return;
            }
            object_.resize(object_size);
            if (!BCRYPT_SUCCESS(BCryptCreateHash(
                    algorithm_,
                    &hash_,
                    object_.data(),
                    static_cast<ULONG>(object_.size()),
                    nullptr,
                    0,
                    0)))
            {
                hash_ = nullptr;
            }
        }

        ~Sha256Hash()
        {
            if (hash_ != nullptr)
            {
                BCryptDestroyHash(hash_);
            }
            if (algorithm_ != nullptr)
            {
                BCryptCloseAlgorithmProvider(algorithm_, 0);
            }
        }

        Sha256Hash(const Sha256Hash&) = delete;
        Sha256Hash& operator=(const Sha256Hash&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return hash_ != nullptr;
        }

        [[nodiscard]] bool append(const void* data, DWORD size) noexcept
        {
            return hash_ != nullptr && BCRYPT_SUCCESS(BCryptHashData(
                hash_,
                static_cast<PUCHAR>(const_cast<void*>(data)),
                size,
                0));
        }

        [[nodiscard]] std::optional<std::wstring> finish() noexcept
        {
            std::array<std::uint8_t, 32> digest{};
            if (hash_ == nullptr)
            {
                return std::nullopt;
            }
            const NTSTATUS status = BCryptFinishHash(
                hash_, digest.data(), static_cast<ULONG>(digest.size()), 0);
            BCryptDestroyHash(hash_);
            hash_ = nullptr;
            if (!BCRYPT_SUCCESS(status))
            {
                return std::nullopt;
            }

            constexpr wchar_t hexadecimal[] = L"0123456789abcdef";
            std::wstring result;
            result.reserve(digest.size() * 2);
            for (const auto value : digest)
            {
                result.push_back(hexadecimal[value >> 4]);
                result.push_back(hexadecimal[value & 0x0f]);
            }
            return result;
        }

    private:
        BCRYPT_ALG_HANDLE algorithm_{};
        BCRYPT_HASH_HANDLE hash_{};
        std::vector<std::uint8_t> object_;
    };

    bool valid_sha256(std::wstring_view value) noexcept
    {
        return value.size() == 64 && std::ranges::all_of(value, [](wchar_t character) {
            return std::iswxdigit(character) != 0;
        });
    }

    void report_progress(
        const glance::app::FileDownloadProgressCallback& progress,
        std::uint64_t downloaded,
        std::uint64_t total) noexcept
    {
        if (!progress)
        {
            return;
        }
        try
        {
            progress(downloaded, total);
        }
        catch (...)
        {
        }
    }

    std::optional<std::wstring> sha256_file(const std::filesystem::path& path) noexcept
    {
        FileHandle file(CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
        Sha256Hash hash;
        if (!file || !hash)
        {
            return std::nullopt;
        }

        std::array<std::uint8_t, download_buffer_bytes> buffer{};
        while (true)
        {
            DWORD read{};
            if (!ReadFile(
                    file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
            {
                return std::nullopt;
            }
            if (read == 0)
            {
                return hash.finish();
            }
            if (!hash.append(buffer.data(), read))
            {
                return std::nullopt;
            }
        }
    }
}

namespace glance::app
{
    FileDownloadResult download_file(
        const FileDownloadRequest& request,
        const std::atomic_bool& cancelled,
        const FileDownloadProgressCallback& progress) noexcept
    {
        std::filesystem::path partial_path;
        try
        {
            if (request.url.empty() || request.destination_path.empty() ||
                !request.destination_path.is_absolute() ||
                request.destination_path.filename().empty() ||
                !valid_sha256(request.sha256) ||
                request.expected_size == 0 || request.maximum_size == 0 ||
                request.expected_size > request.maximum_size)
            {
                return { FileDownloadStatus::integrity_error, {} };
            }

            URL_COMPONENTS components{ sizeof(components) };
            components.dwSchemeLength = static_cast<DWORD>(-1);
            components.dwHostNameLength = static_cast<DWORD>(-1);
            components.dwUrlPathLength = static_cast<DWORD>(-1);
            components.dwExtraInfoLength = static_cast<DWORD>(-1);
            if (!WinHttpCrackUrl(
                    request.url.c_str(),
                    static_cast<DWORD>(request.url.size()),
                    0,
                    &components) ||
                components.nScheme != INTERNET_SCHEME_HTTPS)
            {
                return { FileDownloadStatus::integrity_error, {} };
            }

            std::filesystem::create_directories(request.destination_path.parent_path());
            partial_path = request.destination_path;
            partial_path += L".partial";
            std::error_code error;
            std::filesystem::remove(partial_path, error);
            if (std::filesystem::is_regular_file(request.destination_path, error) &&
                std::filesystem::file_size(request.destination_path, error) ==
                    request.expected_size)
            {
                const auto hash = sha256_file(request.destination_path);
                if (hash && _wcsicmp(hash->c_str(), request.sha256.c_str()) == 0)
                {
                    report_progress(progress, request.expected_size, request.expected_size);
                    return { FileDownloadStatus::succeeded, request.destination_path };
                }
            }
            std::filesystem::remove(request.destination_path, error);

            const std::wstring host(components.lpszHostName, components.dwHostNameLength);
            std::wstring resource_path(components.lpszUrlPath, components.dwUrlPathLength);
            if (components.dwExtraInfoLength > 0)
            {
                resource_path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
            }

            InternetHandle session(WinHttpOpen(
                L"Glance/" GLANCE_VERSION_WSTRING,
                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS,
                0));
            if (!session || !WinHttpSetTimeouts(session.get(), 5000, 5000, 5000, 8000))
            {
                return { FileDownloadStatus::network_error, {} };
            }
            InternetHandle connection(
                WinHttpConnect(session.get(), host.c_str(), components.nPort, 0));
            InternetHandle web_request(connection
                ? WinHttpOpenRequest(
                    connection.get(),
                    L"GET",
                    resource_path.c_str(),
                    nullptr,
                    WINHTTP_NO_REFERER,
                    WINHTTP_DEFAULT_ACCEPT_TYPES,
                    WINHTTP_FLAG_SECURE)
                : nullptr);
            if (!connection || !web_request ||
                !WinHttpSendRequest(
                    web_request.get(),
                    WINHTTP_NO_ADDITIONAL_HEADERS,
                    0,
                    WINHTTP_NO_REQUEST_DATA,
                    0,
                    0,
                    0) ||
                !WinHttpReceiveResponse(web_request.get(), nullptr))
            {
                return { FileDownloadStatus::network_error, {} };
            }

            DWORD status_code{};
            DWORD status_size = sizeof(status_code);
            if (!WinHttpQueryHeaders(
                    web_request.get(),
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX,
                    &status_code,
                    &status_size,
                    WINHTTP_NO_HEADER_INDEX) ||
                status_code != 200)
            {
                return { FileDownloadStatus::network_error, {} };
            }

            FileHandle file(CreateFileW(
                partial_path.c_str(),
                GENERIC_WRITE,
                FILE_SHARE_READ,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN,
                nullptr));
            Sha256Hash hash;
            if (!file || !hash)
            {
                return { FileDownloadStatus::file_error, {} };
            }

            std::array<std::uint8_t, download_buffer_bytes> buffer{};
            std::uint64_t downloaded{};
            auto last_report = std::chrono::steady_clock::now() - std::chrono::seconds(1);
            report_progress(progress, 0, request.expected_size);
            while (!cancelled.load(std::memory_order_acquire))
            {
                DWORD read{};
                if (!WinHttpReadData(
                        web_request.get(),
                        buffer.data(),
                        static_cast<DWORD>(buffer.size()),
                        &read))
                {
                    file.close();
                    std::filesystem::remove(partial_path, error);
                    return { FileDownloadStatus::network_error, {} };
                }
                if (read == 0)
                {
                    break;
                }
                if (downloaded > request.expected_size ||
                    static_cast<std::uint64_t>(read) > request.expected_size - downloaded)
                {
                    file.close();
                    std::filesystem::remove(partial_path, error);
                    return { FileDownloadStatus::integrity_error, {} };
                }

                DWORD written{};
                if (!WriteFile(file.get(), buffer.data(), read, &written, nullptr) ||
                    written != read || !hash.append(buffer.data(), read))
                {
                    file.close();
                    std::filesystem::remove(partial_path, error);
                    return { FileDownloadStatus::file_error, {} };
                }
                downloaded += read;
                const auto now = std::chrono::steady_clock::now();
                if (downloaded == request.expected_size ||
                    now - last_report >= std::chrono::milliseconds(100))
                {
                    report_progress(progress, downloaded, request.expected_size);
                    last_report = now;
                }
            }

            if (cancelled.load(std::memory_order_acquire))
            {
                file.close();
                std::filesystem::remove(partial_path, error);
                return { FileDownloadStatus::cancelled, {} };
            }
            const auto digest = hash.finish();
            file.close();
            if (downloaded != request.expected_size || !digest ||
                _wcsicmp(digest->c_str(), request.sha256.c_str()) != 0)
            {
                std::filesystem::remove(partial_path, error);
                return { FileDownloadStatus::integrity_error, {} };
            }
            report_progress(progress, request.expected_size, request.expected_size);
            if (!MoveFileExW(
                    partial_path.c_str(),
                    request.destination_path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                std::filesystem::remove(partial_path, error);
                return { FileDownloadStatus::file_error, {} };
            }
            return { FileDownloadStatus::succeeded, request.destination_path };
        }
        catch (...)
        {
            std::error_code error;
            if (!partial_path.empty())
            {
                std::filesystem::remove(partial_path, error);
            }
            return { FileDownloadStatus::file_error, {} };
        }
    }
}

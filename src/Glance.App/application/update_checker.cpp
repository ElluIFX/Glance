#include "pch.h"
#include "update_checker.h"
#include "../../version.h"

#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace
{
    constexpr wchar_t github_host[] = L"api.github.com";
    constexpr wchar_t latest_release_path[] = L"/repos/ElluIFX/Glance/releases/latest";
    constexpr wchar_t release_download_prefix[] =
        L"https://github.com/ElluIFX/Glance/releases/download/";
    constexpr wchar_t uninstall_key_path[] =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
        L"{F4A2E1FC-BA77-4A24-83BF-A1D5B90A3E13}_is1";
    constexpr std::size_t maximum_response_bytes = 64 * 1024;
    constexpr std::uint64_t maximum_installer_bytes = 512ULL * 1024 * 1024;
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
                result.push_back(hexadecimal[value & 0x0F]);
            }
            return result;
        }

    private:
        BCRYPT_ALG_HANDLE algorithm_{};
        BCRYPT_HASH_HANDLE hash_{};
        std::vector<std::uint8_t> object_;
    };

    std::optional<std::array<std::uint32_t, 4>> parse_version(std::wstring_view value) noexcept
    {
        if (!value.empty() && (value.front() == L'v' || value.front() == L'V'))
        {
            value.remove_prefix(1);
        }

        std::array<std::uint32_t, 4> parts{};
        std::size_t part_index{};
        std::uint64_t part_value{};
        bool has_digit{};

        for (const wchar_t character : value)
        {
            if (character >= L'0' && character <= L'9')
            {
                has_digit = true;
                part_value = part_value * 10 + static_cast<std::uint32_t>(character - L'0');
                if (part_value > std::numeric_limits<std::uint32_t>::max())
                {
                    return std::nullopt;
                }
                continue;
            }

            if (character != L'.' || !has_digit || part_index >= parts.size() - 1)
            {
                return std::nullopt;
            }
            parts[part_index++] = static_cast<std::uint32_t>(part_value);
            part_value = 0;
            has_digit = false;
        }

        if (!has_digit || part_index < 2)
        {
            return std::nullopt;
        }
        parts[part_index] = static_cast<std::uint32_t>(part_value);
        return parts;
    }

    std::wstring normalized_version(std::wstring_view value)
    {
        if (!value.empty() && (value.front() == L'v' || value.front() == L'V'))
        {
            value.remove_prefix(1);
        }
        return std::wstring(value);
    }

    bool valid_sha256(std::wstring_view value) noexcept
    {
        return value.size() == 64 && std::ranges::all_of(value, [](wchar_t character) {
            return (character >= L'0' && character <= L'9') ||
                (character >= L'a' && character <= L'f') ||
                (character >= L'A' && character <= L'F');
        });
    }

    std::optional<std::wstring> utf8_to_wide(const std::vector<std::uint8_t>& bytes)
    {
        if (bytes.empty() || bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        {
            return std::nullopt;
        }

        const int length = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<int>(bytes.size()),
            nullptr,
            0);
        if (length <= 0)
        {
            return std::nullopt;
        }

        std::wstring value(static_cast<std::size_t>(length), L'\0');
        if (MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<int>(bytes.size()),
                value.data(),
                length) != length)
        {
            return std::nullopt;
        }
        return value;
    }

    bool query_header_present(HINTERNET request, wchar_t const* name) noexcept
    {
        wchar_t value[64]{};
        DWORD size = sizeof(value);
        return WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_CUSTOM,
            name,
            value,
            &size,
            WINHTTP_NO_HEADER_INDEX) != FALSE;
    }

    bool query_header_is_zero(HINTERNET request, wchar_t const* name) noexcept
    {
        wchar_t value[16]{};
        DWORD size = sizeof(value);
        return WinHttpQueryHeaders(
                   request,
                   WINHTTP_QUERY_CUSTOM,
                   name,
                   value,
                   &size,
                   WINHTTP_NO_HEADER_INDEX) != FALSE &&
            wcscmp(value, L"0") == 0;
    }

    bool is_rate_limited(HINTERNET request, DWORD status_code) noexcept
    {
        if (status_code == 429)
        {
            return true;
        }
        if (status_code != 403)
        {
            return false;
        }
        return query_header_present(request, L"Retry-After") ||
            query_header_is_zero(request, L"X-RateLimit-Remaining");
    }

    std::optional<std::vector<std::uint8_t>> read_response(HINTERNET request)
    {
        std::vector<std::uint8_t> response;
        while (true)
        {
            DWORD available{};
            if (!WinHttpQueryDataAvailable(request, &available))
            {
                return std::nullopt;
            }
            if (available == 0)
            {
                return response;
            }
            if (available > maximum_response_bytes - response.size())
            {
                return std::nullopt;
            }

            const std::size_t offset = response.size();
            response.resize(offset + available);
            DWORD read{};
            if (!WinHttpReadData(request, response.data() + offset, available, &read))
            {
                return std::nullopt;
            }
            response.resize(offset + read);
            if (read == 0)
            {
                return response;
            }
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
            if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
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

    std::filesystem::path executable_directory()
    {
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size())
        {
            return {};
        }
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

    std::optional<std::wstring> registry_string(HKEY key, wchar_t const* name)
    {
        DWORD type{};
        DWORD bytes{};
        if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
            (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t))
        {
            return std::nullopt;
        }

        std::wstring value(bytes / sizeof(wchar_t), L'\0');
        if (RegQueryValueExW(
                key,
                name,
                nullptr,
                &type,
                reinterpret_cast<BYTE*>(value.data()),
                &bytes) != ERROR_SUCCESS)
        {
            return std::nullopt;
        }
        while (!value.empty() && value.back() == L'\0')
        {
            value.pop_back();
        }
        if (type == REG_EXPAND_SZ)
        {
            const DWORD expanded_size = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
            if (expanded_size == 0)
            {
                return std::nullopt;
            }
            std::wstring expanded(expanded_size, L'\0');
            if (ExpandEnvironmentStringsW(value.c_str(), expanded.data(), expanded_size) == 0)
            {
                return std::nullopt;
            }
            while (!expanded.empty() && expanded.back() == L'\0')
            {
                expanded.pop_back();
            }
            return expanded;
        }
        return value;
    }

    bool paths_equal(const std::filesystem::path& left, const std::filesystem::path& right)
    {
        const auto normalized_left = std::filesystem::absolute(left).lexically_normal().wstring();
        const auto normalized_right = std::filesystem::absolute(right).lexically_normal().wstring();
        return CompareStringOrdinal(
                   normalized_left.c_str(),
                   static_cast<int>(normalized_left.size()),
                   normalized_right.c_str(),
                   static_cast<int>(normalized_right.size()),
                   TRUE) == CSTR_EQUAL;
    }

    void report_progress(
        const glance::app::UpdateProgressCallback& progress,
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

    void clean_update_directory(
        const std::filesystem::path& root,
        const std::filesystem::path& current_directory)
    {
        std::error_code error;
        if (!std::filesystem::exists(root, error))
        {
            return;
        }
        for (const auto& entry : std::filesystem::directory_iterator(root, error))
        {
            if (error)
            {
                return;
            }
            if (!paths_equal(entry.path(), current_directory))
            {
                std::filesystem::remove_all(entry.path(), error);
                error.clear();
            }
        }
    }
}

namespace glance::app
{
    UpdateCheckResult check_for_updates(std::wstring_view current_version) noexcept
    {
        try
        {
            InternetHandle session(WinHttpOpen(
                L"Glance/" GLANCE_VERSION_WSTRING,
                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS,
                0));
            if (!session || !WinHttpSetTimeouts(session.get(), 5000, 5000, 5000, 8000))
            {
                return {};
            }

            InternetHandle connection(WinHttpConnect(
                session.get(), github_host, INTERNET_DEFAULT_HTTPS_PORT, 0));
            if (!connection)
            {
                return {};
            }

            InternetHandle request(WinHttpOpenRequest(
                connection.get(),
                L"GET",
                latest_release_path,
                nullptr,
                WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES,
                WINHTTP_FLAG_SECURE));
            if (!request)
            {
                return {};
            }

            constexpr wchar_t headers[] =
                L"Accept: application/vnd.github+json\r\n"
                L"X-GitHub-Api-Version: 2022-11-28\r\n";
            if (!WinHttpSendRequest(
                    request.get(),
                    headers,
                    static_cast<DWORD>(-1L),
                    WINHTTP_NO_REQUEST_DATA,
                    0,
                    0,
                    0) ||
                !WinHttpReceiveResponse(request.get(), nullptr))
            {
                return {};
            }

            DWORD status_code{};
            DWORD status_size = sizeof(status_code);
            if (!WinHttpQueryHeaders(
                    request.get(),
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX,
                    &status_code,
                    &status_size,
                    WINHTTP_NO_HEADER_INDEX))
            {
                return {};
            }

            if (is_rate_limited(request.get(), status_code))
            {
                return { UpdateCheckStatus::rate_limited };
            }
            if (status_code == 404)
            {
                return { UpdateCheckStatus::no_release };
            }
            if (status_code != 200)
            {
                return {};
            }

            const auto response = read_response(request.get());
            const auto json_text = response ? utf8_to_wide(*response) : std::nullopt;
            if (!json_text)
            {
                return {};
            }

            const auto json = winrt::Windows::Data::Json::JsonObject::Parse(winrt::hstring(*json_text));
            const std::wstring latest_version(json.GetNamedString(L"tag_name").c_str());
            const auto current = parse_version(current_version);
            const auto latest = parse_version(latest_version);
            if (!current || !latest)
            {
                return {};
            }

            UpdateCheckResult result;
            result.status = *latest > *current
                ? UpdateCheckStatus::update_available
                : UpdateCheckStatus::up_to_date;
            result.latest_version = latest_version;
            result.release_url = json.GetNamedString(L"html_url", L"").c_str();
            if (result.status != UpdateCheckStatus::update_available)
            {
                return result;
            }

            const auto version = normalized_version(latest_version);
            const auto expected_name = L"Glance-Setup-" + version + L"-x64.exe";
            std::optional<UpdateInstallerAsset> installer;
            for (const auto& value : json.GetNamedArray(L"assets"))
            {
                const auto asset = value.GetObjectW();
                if (std::wstring_view(asset.GetNamedString(L"name", L"").c_str()) != expected_name)
                {
                    continue;
                }
                if (installer)
                {
                    return result;
                }

                const std::wstring url(asset.GetNamedString(L"browser_download_url", L"").c_str());
                const std::wstring digest(asset.GetNamedString(L"digest", L"").c_str());
                const double size_value = asset.GetNamedNumber(L"size", 0);
                if (!url.starts_with(release_download_prefix) ||
                    !digest.starts_with(L"sha256:") ||
                    !valid_sha256(std::wstring_view(digest).substr(7)) ||
                    size_value < 1 || size_value > static_cast<double>(maximum_installer_bytes))
                {
                    return result;
                }

                std::wstring sha256 = digest.substr(7);
                std::ranges::transform(sha256, sha256.begin(), [](wchar_t character) {
                    return static_cast<wchar_t>(std::towlower(character));
                });
                installer = UpdateInstallerAsset{
                    version,
                    expected_name,
                    url,
                    std::move(sha256),
                    static_cast<std::uint64_t>(size_value) };
            }
            if (installer)
            {
                result.installer = std::move(*installer);
            }
            return result;
        }
        catch (...)
        {
            return {};
        }
    }

    bool managed_installation() noexcept
    {
        try
        {
            HKEY raw_key{};
            if (RegOpenKeyExW(
                    HKEY_LOCAL_MACHINE,
                    uninstall_key_path,
                    0,
                    KEY_QUERY_VALUE | KEY_WOW64_64KEY,
                    &raw_key) != ERROR_SUCCESS)
            {
                return false;
            }
            const auto install_location = registry_string(raw_key, L"InstallLocation");
            RegCloseKey(raw_key);
            return install_location && !install_location->empty() &&
                paths_equal(*install_location, executable_directory());
        }
        catch (...)
        {
            return false;
        }
    }

    UpdateDownloadResult download_update_installer(
        const UpdateInstallerAsset& asset,
        const std::atomic_bool& cancelled,
        const UpdateProgressCallback& progress) noexcept
    {
        std::filesystem::path partial_path;
        try
        {
            if (!asset || !parse_version(asset.version) ||
                asset.file_name != L"Glance-Setup-" + normalized_version(asset.version) + L"-x64.exe" ||
                !asset.download_url.starts_with(release_download_prefix) ||
                !valid_sha256(asset.sha256) || asset.size > maximum_installer_bytes)
            {
                return { UpdateDownloadStatus::integrity_error, {} };
            }

            const auto root = std::filesystem::temp_directory_path() / L"Glance" / L"Updates";
            const auto version_directory = root / asset.version;
            clean_update_directory(root, version_directory);
            std::filesystem::create_directories(version_directory);
            const auto installer_path = version_directory / asset.file_name;
            partial_path = installer_path;
            partial_path += L".partial";

            std::error_code error;
            std::filesystem::remove(partial_path, error);
            if (std::filesystem::is_regular_file(installer_path, error) &&
                std::filesystem::file_size(installer_path, error) == asset.size)
            {
                const auto hash = sha256_file(installer_path);
                if (hash && *hash == asset.sha256)
                {
                    report_progress(progress, asset.size, asset.size);
                    return { UpdateDownloadStatus::succeeded, installer_path };
                }
            }
            std::filesystem::remove(installer_path, error);

            URL_COMPONENTS components{ sizeof(components) };
            components.dwSchemeLength = static_cast<DWORD>(-1);
            components.dwHostNameLength = static_cast<DWORD>(-1);
            components.dwUrlPathLength = static_cast<DWORD>(-1);
            components.dwExtraInfoLength = static_cast<DWORD>(-1);
            if (!WinHttpCrackUrl(
                    asset.download_url.c_str(),
                    static_cast<DWORD>(asset.download_url.size()),
                    0,
                    &components) ||
                components.nScheme != INTERNET_SCHEME_HTTPS)
            {
                return { UpdateDownloadStatus::integrity_error, {} };
            }

            const std::wstring host(components.lpszHostName, components.dwHostNameLength);
            std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
            if (components.dwExtraInfoLength > 0)
            {
                path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
            }

            InternetHandle session(WinHttpOpen(
                L"Glance/" GLANCE_VERSION_WSTRING,
                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS,
                0));
            if (!session || !WinHttpSetTimeouts(session.get(), 5000, 5000, 5000, 8000))
            {
                return { UpdateDownloadStatus::network_error, {} };
            }
            InternetHandle connection(WinHttpConnect(session.get(), host.c_str(), components.nPort, 0));
            InternetHandle request(connection
                ? WinHttpOpenRequest(
                    connection.get(),
                    L"GET",
                    path.c_str(),
                    nullptr,
                    WINHTTP_NO_REFERER,
                    WINHTTP_DEFAULT_ACCEPT_TYPES,
                    WINHTTP_FLAG_SECURE)
                : nullptr);
            if (!connection || !request ||
                !WinHttpSendRequest(
                    request.get(),
                    WINHTTP_NO_ADDITIONAL_HEADERS,
                    0,
                    WINHTTP_NO_REQUEST_DATA,
                    0,
                    0,
                    0) ||
                !WinHttpReceiveResponse(request.get(), nullptr))
            {
                return { UpdateDownloadStatus::network_error, {} };
            }

            DWORD status_code{};
            DWORD status_size = sizeof(status_code);
            if (!WinHttpQueryHeaders(
                    request.get(),
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX,
                    &status_code,
                    &status_size,
                    WINHTTP_NO_HEADER_INDEX) || status_code != 200)
            {
                return { UpdateDownloadStatus::network_error, {} };
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
                return { UpdateDownloadStatus::file_error, {} };
            }

            std::array<std::uint8_t, download_buffer_bytes> buffer{};
            std::uint64_t downloaded{};
            auto last_report = std::chrono::steady_clock::now() - std::chrono::seconds(1);
            report_progress(progress, 0, asset.size);
            while (!cancelled.load(std::memory_order_acquire))
            {
                DWORD read{};
                if (!WinHttpReadData(
                        request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read))
                {
                    file.close();
                    std::filesystem::remove(partial_path, error);
                    return { UpdateDownloadStatus::network_error, {} };
                }
                if (read == 0)
                {
                    break;
                }
                if (downloaded > asset.size ||
                    static_cast<std::uint64_t>(read) > asset.size - downloaded)
                {
                    file.close();
                    std::filesystem::remove(partial_path, error);
                    return { UpdateDownloadStatus::integrity_error, {} };
                }

                DWORD written{};
                if (!WriteFile(file.get(), buffer.data(), read, &written, nullptr) || written != read ||
                    !hash.append(buffer.data(), read))
                {
                    file.close();
                    std::filesystem::remove(partial_path, error);
                    return { UpdateDownloadStatus::file_error, {} };
                }
                downloaded += read;
                const auto now = std::chrono::steady_clock::now();
                if (downloaded == asset.size || now - last_report >= std::chrono::milliseconds(100))
                {
                    report_progress(progress, downloaded, asset.size);
                    last_report = now;
                }
            }

            if (cancelled.load(std::memory_order_acquire))
            {
                file.close();
                std::filesystem::remove(partial_path, error);
                return { UpdateDownloadStatus::cancelled, {} };
            }
            const auto digest = hash.finish();
            file.close();
            if (downloaded != asset.size || !digest || *digest != asset.sha256)
            {
                std::filesystem::remove(partial_path, error);
                return { UpdateDownloadStatus::integrity_error, {} };
            }
            report_progress(progress, asset.size, asset.size);
            if (!MoveFileExW(
                    partial_path.c_str(),
                    installer_path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                std::filesystem::remove(partial_path, error);
                return { UpdateDownloadStatus::file_error, {} };
            }
            return { UpdateDownloadStatus::succeeded, installer_path };
        }
        catch (...)
        {
            std::error_code error;
            if (!partial_path.empty())
            {
                std::filesystem::remove(partial_path, error);
            }
            return { UpdateDownloadStatus::file_error, {} };
        }
    }

    UpdateLaunchStatus launch_update_installer(const std::filesystem::path& installer_path) noexcept
    {
        try
        {
            if (!std::filesystem::is_regular_file(installer_path))
            {
                return UpdateLaunchStatus::failed;
            }
            constexpr wchar_t parameters[] =
                L"/SP- /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /GLANCEUPDATE";
            SHELLEXECUTEINFOW execute{ sizeof(execute) };
            execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
            execute.lpFile = installer_path.c_str();
            execute.lpParameters = parameters;
            execute.lpDirectory = installer_path.parent_path().c_str();
            execute.nShow = SW_SHOWNORMAL;
            if (!ShellExecuteExW(&execute))
            {
                return GetLastError() == ERROR_CANCELLED
                    ? UpdateLaunchStatus::cancelled
                    : UpdateLaunchStatus::failed;
            }
            if (execute.hProcess == nullptr)
            {
                return UpdateLaunchStatus::failed;
            }

            const DWORD wait_result = WaitForSingleObject(execute.hProcess, INFINITE);
            DWORD exit_code = ERROR_GEN_FAILURE;
            const bool exited = wait_result == WAIT_OBJECT_0 &&
                GetExitCodeProcess(execute.hProcess, &exit_code) != FALSE;
            CloseHandle(execute.hProcess);
            if (!exited)
            {
                return UpdateLaunchStatus::failed;
            }
            if (exit_code == 0)
            {
                return UpdateLaunchStatus::launched;
            }
            return exit_code == 2 || exit_code == 5
                ? UpdateLaunchStatus::cancelled
                : UpdateLaunchStatus::failed;
        }
        catch (...)
        {
            return UpdateLaunchStatus::failed;
        }
    }
}

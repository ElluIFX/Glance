#include "network_service.h"
#include "../../version.h"

#include <windows.h>
#include <winhttp.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <array>
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
    constexpr std::size_t maximum_response_bytes = 64 * 1024;

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

}

namespace glance::core
{
    glance::contracts::UpdateCheckResult check_for_updates(
        std::wstring_view current_version) noexcept
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
                return { glance::contracts::UpdateCheckStatus::rate_limited };
            }
            if (status_code == 404)
            {
                return { glance::contracts::UpdateCheckStatus::no_release };
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

            glance::contracts::UpdateCheckResult result;
            result.status = *latest > *current
                ? glance::contracts::UpdateCheckStatus::update_available
                : glance::contracts::UpdateCheckStatus::up_to_date;
            result.latest_version = latest_version;
            result.release_url = json.GetNamedString(L"html_url", L"").c_str();
            if (result.status != glance::contracts::UpdateCheckStatus::update_available)
            {
                return result;
            }

            const auto version = normalized_version(latest_version);
            const auto expected_name = L"Glance-Setup-" + version + L"-x64.exe";
            std::optional<glance::contracts::UpdateInstallerAsset> installer;
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
                    size_value < 1 ||
                    size_value > static_cast<double>(
                        glance::contracts::maximum_network_download_bytes))
                {
                    return result;
                }

                std::wstring sha256 = digest.substr(7);
                std::ranges::transform(sha256, sha256.begin(), [](wchar_t character) {
                    return static_cast<wchar_t>(std::towlower(character));
                });
                installer = glance::contracts::UpdateInstallerAsset{
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

}

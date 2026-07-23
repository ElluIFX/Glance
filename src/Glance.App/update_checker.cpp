#include "pch.h"
#include "update_checker.h"
#include "../version.h"

#include <winhttp.h>

#include <array>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace
{
    constexpr wchar_t github_host[] = L"api.github.com";
    constexpr wchar_t latest_release_path[] = L"/repos/ElluIFX/Glance/releases/latest";
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
            if (!session)
            {
                return {};
            }
            if (!WinHttpSetTimeouts(session.get(), 5000, 5000, 5000, 8000))
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
                return { UpdateCheckStatus::rate_limited, {} };
            }
            if (status_code == 404)
            {
                return { UpdateCheckStatus::no_release, {} };
            }
            if (status_code != 200)
            {
                return {};
            }

            const auto response = read_response(request.get());
            if (!response)
            {
                return {};
            }
            const auto json_text = utf8_to_wide(*response);
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

            return {
                *latest > *current
                    ? UpdateCheckStatus::update_available
                    : UpdateCheckStatus::up_to_date,
                latest_version };
        }
        catch (...)
        {
            return {};
        }
    }
}

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace glance::contracts
{
    inline constexpr std::uint64_t maximum_network_download_bytes =
        512ULL * 1024ULL * 1024ULL;

    enum class UpdateCheckStatus : std::uint32_t
    {
        update_available,
        up_to_date,
        rate_limited,
        no_release,
        unavailable,
    };

    struct UpdateInstallerAsset
    {
        std::wstring version;
        std::wstring file_name;
        std::wstring download_url;
        std::wstring sha256;
        std::uint64_t size{};

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return !version.empty() && !file_name.empty() && !download_url.empty() &&
                sha256.size() == 64 && size > 0;
        }
    };

    struct UpdateCheckResult
    {
        UpdateCheckStatus status{ UpdateCheckStatus::unavailable };
        std::wstring latest_version;
        std::wstring release_url;
        UpdateInstallerAsset installer;
        std::uint64_t checked_at{};
    };

    struct UpdateCheckRequest
    {
        std::uint64_t request_id{};
        std::uint64_t last_successful_check{};
        bool automatic{};
        std::wstring current_version;
    };

    struct UpdateCheckResponse
    {
        std::uint64_t request_id{};
        UpdateCheckResult result;
    };

    enum class NetworkDownloadStatus : std::uint32_t
    {
        succeeded,
        cancelled,
        network_error,
        file_error,
        integrity_error,
    };

    struct NetworkDownloadRequest
    {
        std::wstring url;
        std::wstring file_name;
        std::wstring sha256;
        std::uint64_t expected_size{};
    };

    struct NetworkDownloadResult
    {
        NetworkDownloadStatus status{ NetworkDownloadStatus::network_error };
        std::wstring path;
    };

    struct NetworkDownloadMessage
    {
        std::uint64_t request_id{};
        NetworkDownloadRequest request;
    };

    struct NetworkDownloadProgress
    {
        std::uint64_t request_id{};
        std::uint64_t downloaded{};
        std::uint64_t total{};
    };

    struct NetworkDownloadResponse
    {
        std::uint64_t request_id{};
        NetworkDownloadResult result;
    };

    [[nodiscard]] constexpr bool update_check_succeeded(UpdateCheckStatus status) noexcept
    {
        return status == UpdateCheckStatus::update_available ||
            status == UpdateCheckStatus::up_to_date;
    }

    [[nodiscard]] constexpr bool cached_update_result_is_newer(
        std::uint64_t checked_at,
        std::uint64_t last_successful_check) noexcept
    {
        return checked_at != 0 && checked_at > last_successful_check;
    }

    [[nodiscard]] constexpr bool should_publish_automatic_update_result(
        UpdateCheckStatus status,
        bool preview_active) noexcept
    {
        return !update_check_succeeded(status) || preview_active;
    }

    [[nodiscard]] std::string encode_update_check_request(const UpdateCheckRequest& value);
    [[nodiscard]] std::optional<UpdateCheckRequest> decode_update_check_request(
        std::string_view payload) noexcept;
    [[nodiscard]] std::string encode_update_check_response(const UpdateCheckResponse& value);
    [[nodiscard]] std::optional<UpdateCheckResponse> decode_update_check_response(
        std::string_view payload) noexcept;
    [[nodiscard]] std::string encode_update_check_deferred(std::uint64_t request_id);
    [[nodiscard]] std::optional<std::uint64_t> decode_update_check_deferred(
        std::string_view payload) noexcept;
    [[nodiscard]] std::string encode_network_download_request(const NetworkDownloadMessage& value);
    [[nodiscard]] std::optional<NetworkDownloadMessage> decode_network_download_request(
        std::string_view payload) noexcept;
    [[nodiscard]] std::string encode_network_download_progress(const NetworkDownloadProgress& value);
    [[nodiscard]] std::optional<NetworkDownloadProgress> decode_network_download_progress(
        std::string_view payload) noexcept;
    [[nodiscard]] std::string encode_network_download_response(const NetworkDownloadResponse& value);
    [[nodiscard]] std::optional<NetworkDownloadResponse> decode_network_download_response(
        std::string_view payload) noexcept;
    [[nodiscard]] std::string encode_network_download_cancel(std::uint64_t request_id);
    [[nodiscard]] std::optional<std::uint64_t> decode_network_download_cancel(
        std::string_view payload) noexcept;
}

#pragma once

#include "download_service.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace glance::app
{
    enum class UpdateCheckStatus
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
    };

    using UpdateDownloadStatus = FileDownloadStatus;

    struct UpdateDownloadResult
    {
        UpdateDownloadStatus status{ UpdateDownloadStatus::network_error };
        std::filesystem::path installer_path;
    };

    enum class UpdateLaunchStatus
    {
        launched,
        cancelled,
        failed,
    };

    using UpdateProgressCallback = FileDownloadProgressCallback;

    [[nodiscard]] UpdateCheckResult check_for_updates(std::wstring_view current_version) noexcept;
    [[nodiscard]] bool managed_installation() noexcept;
    [[nodiscard]] UpdateDownloadResult download_update_installer(
        const UpdateInstallerAsset& asset,
        const std::atomic_bool& cancelled,
        const UpdateProgressCallback& progress) noexcept;
    [[nodiscard]] UpdateLaunchStatus launch_update_installer(
        const std::filesystem::path& installer_path) noexcept;
}

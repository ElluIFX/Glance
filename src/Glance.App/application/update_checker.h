#pragma once

#include "glance/contracts/network_protocol.h"

#include <filesystem>

namespace glance::app
{
    using UpdateCheckStatus = glance::contracts::UpdateCheckStatus;
    using UpdateInstallerAsset = glance::contracts::UpdateInstallerAsset;
    using UpdateCheckResult = glance::contracts::UpdateCheckResult;

    enum class UpdateLaunchStatus
    {
        launched,
        cancelled,
        failed,
    };

    [[nodiscard]] UpdateLaunchStatus launch_update_installer(
        const std::filesystem::path& installer_path, std::wstring_view version,
        bool wait_for_completion = true) noexcept;
}

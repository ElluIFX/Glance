#pragma once

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

    struct UpdateCheckResult
    {
        UpdateCheckStatus status{ UpdateCheckStatus::unavailable };
        std::wstring latest_version;
    };

    [[nodiscard]] UpdateCheckResult check_for_updates(std::wstring_view current_version) noexcept;
}

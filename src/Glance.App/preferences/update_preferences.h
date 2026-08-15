#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace glance::app
{
    enum class UpdateCheckFrequency : std::uint32_t
    {
        hourly,
        daily,
        weekly,
        monthly,
    };

    struct UpdatePreferences
    {
        bool automatic_check_enabled{ true };
        UpdateCheckFrequency frequency{ UpdateCheckFrequency::daily };
        std::uint64_t last_successful_check{};
        std::uint64_t retry_after{};
        std::wstring skipped_version;
    };

    [[nodiscard]] constexpr std::uint64_t update_check_interval_seconds(
        UpdateCheckFrequency frequency) noexcept
    {
        switch (frequency)
        {
        case UpdateCheckFrequency::hourly:
            return 60ULL * 60ULL;
        case UpdateCheckFrequency::weekly:
            return 7ULL * 24ULL * 60ULL * 60ULL;
        case UpdateCheckFrequency::monthly:
            return 30ULL * 24ULL * 60ULL * 60ULL;
        default:
            return 24ULL * 60ULL * 60ULL;
        }
    }

    [[nodiscard]] constexpr bool automatic_update_check_due(
        const UpdatePreferences& preferences,
        std::uint64_t now) noexcept
    {
        if (!preferences.automatic_check_enabled || now < preferences.retry_after)
        {
            return false;
        }
        if (preferences.last_successful_check == 0)
        {
            return true;
        }
        return now >= preferences.last_successful_check &&
            now - preferences.last_successful_check >=
                update_check_interval_seconds(preferences.frequency);
    }

    [[nodiscard]] UpdatePreferences load_update_preferences() noexcept;
    void save_update_preferences(const UpdatePreferences& preferences) noexcept;
    [[nodiscard]] constexpr bool update_version_is_skipped(
        const UpdatePreferences& preferences,
        std::wstring_view version) noexcept
    {
        if (preferences.skipped_version.empty() ||
            preferences.skipped_version.size() != version.size())
        {
            return false;
        }
        for (std::size_t index = 0; index < version.size(); ++index)
        {
            const auto fold_ascii = [](wchar_t value) constexpr {
                return value >= L'A' && value <= L'Z'
                    ? static_cast<wchar_t>(value + (L'a' - L'A'))
                    : value;
            };
            if (fold_ascii(preferences.skipped_version[index]) !=
                fold_ascii(version[index]))
            {
                return false;
            }
        }
        return true;
    }
}

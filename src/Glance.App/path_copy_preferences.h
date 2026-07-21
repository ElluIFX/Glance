#pragma once

namespace glance::app
{
    struct PathCopyPreferences
    {
        bool quote_path{};
        bool use_unix_separators{};
    };

    [[nodiscard]] PathCopyPreferences load_path_copy_preferences() noexcept;
    void save_path_copy_preferences(const PathCopyPreferences& preferences) noexcept;
}

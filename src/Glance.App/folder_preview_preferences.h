#pragma once

#include <cstdint>

namespace glance::app
{
    enum class FolderSortField : std::uint32_t
    {
        name,
        type,
        modified_time,
        size,
    };

    struct FolderPreviewPreferences
    {
        FolderSortField sort_field{ FolderSortField::name };
        bool ascending{ true };
    };

    [[nodiscard]] FolderPreviewPreferences load_folder_preview_preferences() noexcept;
    void save_folder_preview_preferences(const FolderPreviewPreferences& preferences) noexcept;
}

#pragma once

namespace glance::app
{
    struct GenericPreviewPreferences
    {
        bool show_advanced_info{};
    };

    [[nodiscard]] GenericPreviewPreferences load_generic_preview_preferences() noexcept;
    void save_generic_preview_preferences(const GenericPreviewPreferences& preferences) noexcept;
}

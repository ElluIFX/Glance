#pragma once

#include <cstdint>

namespace glance::app
{
    struct OfficePreviewPreferences
    {
        std::uint32_t cache_capacity{ 1 };
        std::uint32_t cache_expiration_minutes{ 5 };
    };

    [[nodiscard]] OfficePreviewPreferences load_office_preview_preferences() noexcept;
    void save_office_preview_preferences(const OfficePreviewPreferences& preferences) noexcept;
}

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace glance::app
{
    enum class FooterField : std::uint32_t
    {
        size,
        modified_time,
        creation_time,
        permissions,
        media_info,
        taken_time,
    };

    constexpr std::size_t footer_field_count = 6;

    struct FooterPreferences
    {
        std::array<FooterField, footer_field_count> order{
            FooterField::size,
            FooterField::modified_time,
            FooterField::taken_time,
            FooterField::creation_time,
            FooterField::permissions,
            FooterField::media_info,
        };
        std::uint32_t enabled_mask{
            (1U << static_cast<std::uint32_t>(FooterField::size)) |
            (1U << static_cast<std::uint32_t>(FooterField::modified_time)) |
            (1U << static_cast<std::uint32_t>(FooterField::media_info))
        };
    };

    [[nodiscard]] constexpr std::uint32_t footer_field_bit(FooterField field) noexcept
    {
        return 1U << static_cast<std::uint32_t>(field);
    }

    [[nodiscard]] constexpr bool footer_field_enabled(
        const FooterPreferences& preferences,
        FooterField field) noexcept
    {
        return (preferences.enabled_mask & footer_field_bit(field)) != 0;
    }

    [[nodiscard]] FooterPreferences load_footer_preferences() noexcept;
    void save_footer_preferences(const FooterPreferences& preferences) noexcept;
}

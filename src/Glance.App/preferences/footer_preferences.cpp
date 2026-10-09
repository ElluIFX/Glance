#include "pch.h"
#include "glance/contracts/storage.h"
#include "footer_preferences.h"
#include "public_settings.h"

#include <array>

namespace
{
    const glance::app::RegisterPublicSettings public_settings{
        { L"Footer/EnabledFields", L"integer", L"243", 0, 255, L"", L"immediate" },
        { L"Footer/FieldOrder", L"array", L"[0,7,4,5,6,1,2,3]", 0, 7, L"", L"immediate" },
    };
    constexpr wchar_t registry_path[] = L"Software\\Glance\\Footer";
    constexpr std::size_t legacy_field_count = 4;
    constexpr std::size_t previous_field_count = 5;
    constexpr std::uint32_t all_fields_mask =
        (1U << static_cast<std::uint32_t>(glance::app::footer_field_count)) - 1U;

    bool valid_order(const std::array<glance::app::FooterField, glance::app::footer_field_count>& order) noexcept
    {
        std::uint32_t seen{};
        for (const auto field : order)
        {
            const auto value = static_cast<std::uint32_t>(field);
            if (value >= glance::app::footer_field_count || (seen & (1U << value)) != 0)
            {
                return false;
            }
            seen |= 1U << value;
        }
        return seen == all_fields_mask;
    }

    bool valid_legacy_order(
        const std::array<glance::app::FooterField, legacy_field_count>& order) noexcept
    {
        std::uint32_t seen{};
        for (const auto field : order)
        {
            const auto value = static_cast<std::uint32_t>(field);
            if (value >= legacy_field_count || (seen & (1U << value)) != 0)
            {
                return false;
            }
            seen |= 1U << value;
        }
        return seen == (1U << legacy_field_count) - 1U;
    }

    bool valid_previous_order(
        const std::array<glance::app::FooterField, previous_field_count>& order) noexcept
    {
        std::uint32_t seen{};
        for (const auto field : order)
        {
            const auto value = static_cast<std::uint32_t>(field);
            if (value >= previous_field_count || (seen & (1U << value)) != 0)
            {
                return false;
            }
            seen |= 1U << value;
        }
        return seen == (1U << previous_field_count) - 1U;
    }
}

namespace glance::app
{
    FooterPreferences load_footer_preferences() noexcept
    {
        FooterPreferences result;
        DWORD mask = result.enabled_mask;
        DWORD mask_size = sizeof(mask);
        if (glance::contracts::storage::read_value(registry_path, L"EnabledFields", REG_DWORD, &mask, &mask_size) == ERROR_SUCCESS)
        {
            result.enabled_mask = mask & all_fields_mask;
        }

        std::array<FooterField, 10> order{};
        DWORD order_size = static_cast<DWORD>(sizeof(order));
        const LSTATUS order_status = glance::contracts::storage::read_value(registry_path, L"FieldOrder", REG_BINARY, order.data(), &order_size);
        if (order_status == ERROR_SUCCESS && order_size == sizeof(result.order))
        {
            std::array<FooterField, footer_field_count> current_order{};
            std::copy_n(order.begin(), current_order.size(), current_order.begin());
            if (valid_order(current_order)) result.order = current_order;
        }
        else if (order_status == ERROR_SUCCESS && order_size == sizeof(order))
        {
            std::uint32_t seen{};
            std::size_t output_index{};
            auto merged_order = result.order;
            for (const auto field : order)
            {
                const auto value = static_cast<std::uint32_t>(field);
                if (value >= order.size() || (seen & (1U << value)) != 0) break;
                seen |= 1U << value;
                if (value != 7 && value != 8)
                    merged_order[output_index++] = value == 9 ? FooterField::line_endings : field;
            }
            if (seen == 1023)
            {
                result.order = merged_order;
                result.enabled_mask = (mask & 127U) | ((mask & (1U << 9)) >> 2);
                if (mask & (1U << 7)) result.enabled_mask |= footer_field_bit(FooterField::media_info);
                if (mask & (1U << 8)) result.enabled_mask |= footer_field_bit(FooterField::capture_parameters);
            }
        }
        else if (order_status == ERROR_SUCCESS && order_size == 6 * sizeof(FooterField))
        {
            std::uint32_t seen{};
            bool valid = true;
            for (std::size_t index = 0; index < 6; ++index)
            {
                const auto value = static_cast<std::uint32_t>(order[index]);
                if (value >= 6 || (seen & (1U << value)) != 0)
                {
                    valid = false;
                    break;
                }
                seen |= 1U << value;
            }
            if (valid)
            {
                std::copy_n(order.begin(), 6, result.order.begin());
                result.order[6] = FooterField::capture_parameters;
                result.order[7] = FooterField::line_endings;
            }
        }
        else if (order_status == ERROR_SUCCESS &&
                 order_size == previous_field_count * sizeof(FooterField))
        {
            std::array<FooterField, previous_field_count> previous_order{};
            std::copy_n(order.begin(), previous_order.size(), previous_order.begin());
            if (valid_previous_order(previous_order))
            {
                result.order[6] = FooterField::capture_parameters;
                result.order[7] = FooterField::line_endings;
                std::size_t output_index{};
                for (const auto field : previous_order)
                {
                    result.order[output_index++] = field;
                    if (field == FooterField::modified_time)
                    {
                        result.order[output_index++] = FooterField::taken_time;
                    }
                }
            }
        }
        else if (order_status == ERROR_SUCCESS &&
                 order_size == legacy_field_count * sizeof(FooterField))
        {
            std::array<FooterField, legacy_field_count> legacy_order{};
            std::copy_n(order.begin(), legacy_order.size(), legacy_order.begin());
            if (valid_legacy_order(legacy_order))
            {
                std::size_t output_index{};
                for (const auto field : legacy_order)
                {
                    result.order[output_index++] = field;
                    if (field == FooterField::modified_time)
                    {
                        result.order[output_index++] = FooterField::taken_time;
                    }
                }
                result.order[output_index] = FooterField::media_info;
                result.order[6] = FooterField::capture_parameters;
                result.order[7] = FooterField::line_endings;
                result.enabled_mask |= footer_field_bit(FooterField::media_info);
            }
        }
        return result;
    }

    void save_footer_preferences(const FooterPreferences& preferences) noexcept
    {
        if (!valid_order(preferences.order))
        {
            return;
        }

        glance::contracts::storage::Batch key(registry_path);

        const DWORD enabled_mask = preferences.enabled_mask & all_fields_mask;
        key.set(L"EnabledFields", REG_DWORD, reinterpret_cast<const BYTE*>(&enabled_mask), sizeof(enabled_mask));
        key.set(L"FieldOrder", REG_BINARY, reinterpret_cast<const BYTE*>(preferences.order.data()), static_cast<DWORD>(sizeof(preferences.order)));
        static_cast<void>(key.commit());
    }
}

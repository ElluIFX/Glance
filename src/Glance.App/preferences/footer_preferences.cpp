#include "pch.h"
#include "footer_preferences.h"

#include <array>

namespace
{
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
        DWORD mask{};
        DWORD mask_size = sizeof(mask);
        if (RegGetValueW(
                HKEY_CURRENT_USER,
                registry_path,
                L"EnabledFields",
                RRF_RT_REG_DWORD,
                nullptr,
                &mask,
                &mask_size) == ERROR_SUCCESS)
        {
            result.enabled_mask = mask & all_fields_mask;
        }

        std::array<FooterField, footer_field_count> order{};
        DWORD order_size = static_cast<DWORD>(sizeof(order));
        const LSTATUS order_status = RegGetValueW(
                HKEY_CURRENT_USER,
                registry_path,
                L"FieldOrder",
                RRF_RT_REG_BINARY,
                nullptr,
                order.data(),
                &order_size);
        if (order_status == ERROR_SUCCESS && order_size == sizeof(order) && valid_order(order))
        {
            result.order = order;
        }
        else if (order_status == ERROR_SUCCESS &&
                 order_size == previous_field_count * sizeof(FooterField))
        {
            std::array<FooterField, previous_field_count> previous_order{};
            std::copy_n(order.begin(), previous_order.size(), previous_order.begin());
            if (valid_previous_order(previous_order))
            {
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

        HKEY key{};
        if (RegCreateKeyExW(
                HKEY_CURRENT_USER,
                registry_path,
                0,
                nullptr,
                0,
                KEY_SET_VALUE,
                nullptr,
                &key,
                nullptr) != ERROR_SUCCESS)
        {
            return;
        }

        const DWORD enabled_mask = preferences.enabled_mask & all_fields_mask;
        RegSetValueExW(
            key,
            L"EnabledFields",
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&enabled_mask),
            sizeof(enabled_mask));
        RegSetValueExW(
            key,
            L"FieldOrder",
            0,
            REG_BINARY,
            reinterpret_cast<const BYTE*>(preferences.order.data()),
            static_cast<DWORD>(sizeof(preferences.order)));
        RegCloseKey(key);
    }
}

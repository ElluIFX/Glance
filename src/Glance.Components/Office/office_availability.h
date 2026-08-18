#pragma once

#include <string_view>

namespace glance::app
{
    inline constexpr unsigned int office_all_components = 0x7U;

    void initialize_office_availability() noexcept;
    [[nodiscard]] unsigned int office_available_components() noexcept;
    [[nodiscard]] bool office_preview_available(std::wstring_view path) noexcept;
}

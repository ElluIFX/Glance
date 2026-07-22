#pragma once

#include <string_view>

namespace glance::app
{
    void initialize_office_availability() noexcept;
    [[nodiscard]] bool office_com_available() noexcept;
    [[nodiscard]] bool office_preview_available(std::wstring_view path) noexcept;
}

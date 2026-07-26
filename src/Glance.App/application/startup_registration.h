#pragma once

#include <string_view>

namespace glance::app
{
    [[nodiscard]] bool launch_at_sign_in_enabled() noexcept;
    [[nodiscard]] bool set_launch_at_sign_in(bool enabled) noexcept;
    [[nodiscard]] bool cleanup_launch_at_sign_in(std::wstring_view executable_path) noexcept;
}

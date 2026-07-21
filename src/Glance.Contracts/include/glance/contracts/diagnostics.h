#pragma once

#include <string_view>

namespace glance::contracts
{
    [[nodiscard]] bool diagnostics_enabled() noexcept;
    void set_diagnostics_enabled(bool enabled) noexcept;
    void initialize_diagnostics(std::wstring_view process_name) noexcept;
    void log_event(std::wstring_view message) noexcept;
}

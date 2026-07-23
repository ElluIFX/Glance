#pragma once

namespace glance::app
{
    void initialize_webview_availability() noexcept;
    void refresh_webview_availability() noexcept;
    [[nodiscard]] bool webview_runtime_available() noexcept;
}

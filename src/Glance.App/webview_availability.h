#pragma once

#include <winrt/Microsoft.Web.WebView2.Core.h>

namespace glance::app
{
    void initialize_webview_availability() noexcept;
    void refresh_webview_availability() noexcept;
    [[nodiscard]] bool webview_runtime_available() noexcept;
    [[nodiscard]] winrt::Windows::Foundation::IAsyncOperation<
        winrt::Microsoft::Web::WebView2::Core::CoreWebView2Environment>
        shared_webview_environment_async();
}

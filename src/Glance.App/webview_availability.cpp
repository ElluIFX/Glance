#include "pch.h"
#include "webview_availability.h"

#include <winrt/Microsoft.Web.WebView2.Core.h>

#include <atomic>

namespace
{
    std::atomic_bool runtime_available{};

    bool detect_webview_runtime() noexcept
    {
        try
        {
            return !winrt::Microsoft::Web::WebView2::Core::CoreWebView2Environment::
                GetAvailableBrowserVersionString().empty();
        }
        catch (...)
        {
            return false;
        }
    }
}

namespace glance::app
{
    void initialize_webview_availability() noexcept
    {
        refresh_webview_availability();
    }

    void refresh_webview_availability() noexcept
    {
        runtime_available.store(detect_webview_runtime(), std::memory_order_release);
    }

    bool webview_runtime_available() noexcept
    {
        return runtime_available.load(std::memory_order_acquire);
    }
}

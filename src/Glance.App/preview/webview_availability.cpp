#include "pch.h"
#include "glance/contracts/storage.h"
#include "webview_availability.h"

#include <shlobj.h>
#include <winrt/Microsoft.Web.WebView2.Core.h>

#include <atomic>
#include <filesystem>
#include <memory>

namespace
{
    std::atomic_bool runtime_available{};
    winrt::Microsoft::Web::WebView2::Core::CoreWebView2Environment shared_environment{
        nullptr
    };

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

    winrt::Windows::Foundation::IAsyncOperation<
        winrt::Microsoft::Web::WebView2::Core::CoreWebView2Environment>
        shared_webview_environment_async()
    {
        if (shared_environment != nullptr)
        {
            co_return shared_environment;
        }

        const auto user_data_folder = contracts::storage::data_directory() / L"WebView2";
        std::filesystem::create_directories(user_data_folder);

        const auto environment =
            co_await winrt::Microsoft::Web::WebView2::Core::CoreWebView2Environment::
                CreateWithOptionsAsync(
                    winrt::hstring{},
                    user_data_folder.c_str(),
                    nullptr);
        if (shared_environment == nullptr)
        {
            shared_environment = environment;
        }
        co_return shared_environment;
    }
}

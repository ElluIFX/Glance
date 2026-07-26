#include "pch.h"
#include "webview_availability.h"

#include <shlobj.h>
#include <winrt/Microsoft.Web.WebView2.Core.h>

#include <atomic>
#include <filesystem>
#include <memory>

namespace
{
    struct CoTaskMemDeleter
    {
        void operator()(void* value) const noexcept
        {
            CoTaskMemFree(value);
        }
    };

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

        PWSTR local_app_data_raw = nullptr;
        winrt::check_hresult(SHGetKnownFolderPath(
            FOLDERID_LocalAppData,
            KF_FLAG_CREATE,
            nullptr,
            &local_app_data_raw));
        const std::unique_ptr<wchar_t, CoTaskMemDeleter> local_app_data(
            local_app_data_raw);
        const std::filesystem::path user_data_folder =
            std::filesystem::path(local_app_data.get()) / L"Glance" / L"WebView2";
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

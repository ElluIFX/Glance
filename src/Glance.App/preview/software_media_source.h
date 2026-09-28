#pragma once
#include <memory>
#include <atomic>
#include <string>
#include <winrt/Windows.Media.Core.h>

namespace glance::app
{
    class SoftwareMediaSource
    {
    public:
        virtual ~SoftwareMediaSource() = default;
        virtual winrt::Windows::Media::Core::MediaSource source() const = 0;
        virtual void cancel() noexcept = 0;
        static std::shared_ptr<SoftwareMediaSource> open(const std::wstring& path,
            std::shared_ptr<std::atomic_bool> cancellation = {});
    };
}

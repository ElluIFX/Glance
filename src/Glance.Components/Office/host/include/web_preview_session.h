#pragma once

#include "glance/contracts/native_preview_protocol.h"
#include <windows.h>
#include <functional>
#include <memory>
#include <string>

namespace glance::office
{
    class WebPreviewSession final
    {
    public:
        WebPreviewSession() = default;
        WebPreviewSession(const WebPreviewSession&) = delete;
        WebPreviewSession& operator=(const WebPreviewSession&) = delete;
        ~WebPreviewSession();
        bool available(const std::wstring& path) const;
        glance::contracts::native_preview::Status open(
            const std::wstring& path, HWND parent, const RECT& bounds,
            const glance::contracts::native_preview::PreviewVisuals& visuals,
            HANDLE cancellation, std::function<void()> failure);
        bool active() const noexcept;
        glance::contracts::native_preview::ContentSize content_size() const noexcept;
        void resize(const RECT& bounds);
        void set_visuals(const glance::contracts::native_preview::PreviewVisuals& visuals);
        void close() noexcept;

    private:
        struct State;
        std::shared_ptr<State> state_;
    };
}

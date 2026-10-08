#pragma once
#include "access_wire.h"
#include "unique_handle.h"
#include "glance/contracts/access_runtime.h"
#include <mutex>
#include <thread>

namespace glance::core
{
    class AccessClient
    {
    public:
        explicit AccessClient(HWND notification_window);
        ~AccessClient();
        AccessClient(const AccessClient&) = delete;
        AccessClient& operator=(const AccessClient&) = delete;
        [[nodiscard]] bool ready() const noexcept;
        void update_input(const contracts::access::InputState& source) noexcept;
        [[nodiscard]] contracts::SelectionSnapshot selection(bool& suppress);
        [[nodiscard]] GalleryResponse gallery(const GalleryCommand& command);
        void request_hook_refresh() noexcept { refresh_requested_.store(true); }
        [[nodiscard]] std::uint64_t hook_events() const noexcept;
        [[nodiscard]] static bool requires_access(HWND window) noexcept;
    private:
        void ensure_connected();
        [[nodiscard]] std::string exchange(contracts::access::Operation operation, std::string payload);
        void disconnect() noexcept;
        HWND window_{};
        unique_handle mapping_, lease_, pipe_;
        contracts::access::SharedState* state_{};
        std::mutex mutex_;
        std::atomic_bool connected_{}, refresh_requested_{};
        ULONGLONG retry_at_{};
        std::jthread worker_;
    };
}

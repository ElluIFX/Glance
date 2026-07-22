#pragma once

#include "unique_handle.h"

#include <windows.h>

#include <cstdint>
#include <string>

namespace glance::core
{
    class DialogHookClient final
    {
    public:
        DialogHookClient() = default;
        ~DialogHookClient();

        DialogHookClient(const DialogHookClient&) = delete;
        DialogHookClient& operator=(const DialogHookClient&) = delete;

        [[nodiscard]] std::wstring query(HWND dialog_window, DWORD process_id, DWORD thread_id);
        void detach() noexcept;

    private:
        enum class TargetArchitecture
        {
            native,
            x86,
            unsupported,
        };

        [[nodiscard]] TargetArchitecture target_architecture(DWORD process_id) const;

        [[nodiscard]] bool load_native_hook();
        [[nodiscard]] bool attach_native_hook(DWORD process_id, DWORD thread_id);
        [[nodiscard]] std::wstring query_native_hook(HWND dialog_window, DWORD process_id, DWORD thread_id);
        void detach_native_hook() noexcept;

        [[nodiscard]] bool start_broker();
        [[nodiscard]] bool exchange_with_broker(
            std::uint32_t command,
            HWND dialog_window,
            DWORD process_id,
            DWORD thread_id,
            std::wstring& path);
        [[nodiscard]] std::wstring query_broker(HWND dialog_window, DWORD process_id, DWORD thread_id);
        void detach_broker() noexcept;
        void stop_broker() noexcept;

        HMODULE module_{};
        HHOOK native_hook_{};
        DWORD native_hooked_process_id_{};
        DWORD native_hooked_thread_id_{};
        UINT query_message_{};
        LRESULT(CALLBACK* hook_proc_)(int, WPARAM, LPARAM){};
        void(WINAPI* prepare_)(DWORD, DWORD){};
        UINT(WINAPI* read_path_)(PWSTR, UINT){};

        unique_handle broker_process_;
        unique_handle broker_request_pipe_;
        unique_handle broker_response_pipe_;
        bool broker_hook_attached_{};
    };
}

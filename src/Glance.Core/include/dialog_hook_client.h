#pragma once

#include <windows.h>

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
        [[nodiscard]] bool load();
        [[nodiscard]] bool supports_target(DWORD process_id) const;
        [[nodiscard]] bool attach(DWORD process_id, DWORD thread_id);

        HMODULE module_{};
        HHOOK hook_{};
        DWORD hooked_process_id_{};
        DWORD hooked_thread_id_{};
        UINT query_message_{};
        LRESULT(CALLBACK* hook_proc_)(int, WPARAM, LPARAM){};
        void(WINAPI* prepare_)(DWORD, DWORD){};
        UINT(WINAPI* read_path_)(PWSTR, UINT){};
    };
}

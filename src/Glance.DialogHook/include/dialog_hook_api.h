#pragma once

#include <windows.h>

namespace glance::dialog_hook
{
    inline constexpr wchar_t query_message_name[] = L"Glance.DialogHook.Query.v1";
    inline constexpr char hook_proc_export[] = "GlanceDialogHookProc";
    inline constexpr char prepare_export[] = "GlanceDialogHookPrepare";
    inline constexpr char read_path_export[] = "GlanceDialogHookReadPath";

    using HookProcedure = LRESULT(CALLBACK*)(int code, WPARAM wparam, LPARAM lparam);
    using PrepareFunction = void(WINAPI*)(DWORD process_id, DWORD thread_id);
    using ReadPathFunction = UINT(WINAPI*)(PWSTR buffer, UINT capacity);
}

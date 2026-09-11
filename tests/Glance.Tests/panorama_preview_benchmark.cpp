#include "panorama_preview_benchmark.h"
#include "native_preview_surface.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <string>

namespace
{
    using namespace glance::contracts::native_preview;

    template <typename Function>
    auto pump(Function&& function)
    {
        auto future = std::async(std::launch::async, std::forward<Function>(function));
        while (future.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready)
        {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        return future.get();
    }

    HANDLE find_host()
    {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return nullptr;
        PROCESSENTRY32W entry{ sizeof(entry) };
        HANDLE host{};
        for (BOOL found = Process32FirstW(snapshot, &entry); found;
            found = Process32NextW(snapshot, &entry))
        {
            if (entry.th32ParentProcessID == GetCurrentProcessId() &&
                _wcsicmp(entry.szExeFile, L"Glance.PanoramaVideoHost.exe") == 0)
            {
                host = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE,
                    entry.th32ProcessID);
                break;
            }
        }
        CloseHandle(snapshot);
        return host;
    }

    std::uint64_t cpu_ticks(HANDLE process)
    {
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) return 0;
        return (static_cast<std::uint64_t>(kernel.dwHighDateTime) << 32) + kernel.dwLowDateTime +
            (static_cast<std::uint64_t>(user.dwHighDateTime) << 32) + user.dwLowDateTime;
    }

    bool wait_ready(glance::app::NativePreviewSurface& surface, const wchar_t* stage)
    {
        const auto start = GetTickCount64();
        do
        {
            const auto state = pump([&] { return surface.media_state(); });
            if (!state || (state->flags & media_state_failed) != 0)
            {
                std::wcout << stage << L" failed: " << (state ? state->failure_kind : 0)
                           << L" HRESULT " << (state ? state->failure_hresult : 0) << std::endl;
                return false;
            }
            if ((state->flags & media_state_ready) != 0)
            {
                std::wcout << stage << L" ready_ms=" << GetTickCount64() - start << std::endl;
                return true;
            }
            if (GetTickCount64() - start >= 20000)
            {
                std::wcout << stage << L" timeout flags=" << state->flags
                           << L" position=" << state->position_ticks << std::endl;
                return false;
            }
            Sleep(20);
        } while (true);
    }

    bool measure(const std::wstring& path, const std::wstring& host_path)
    {
        std::wcout << path << std::endl;
        const HWND parent = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC",
            L"Glance panorama benchmark", WS_POPUP, 0, 0, 640, 480,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (parent == nullptr) return false;
        ShowWindow(parent, SW_SHOWNOACTIVATE);
        bool success{};
        {
            glance::app::NativePreviewSurface surface(parent, host_path, nullptr, [] {});
            surface.set_bounds(0, 0, 640, 480);
            surface.set_visible(true);
            success = pump([&] {
                return surface.open(path, {}, 96) == Status::success &&
                    surface.media_set_muted(true) && surface.media_play();
            }) && wait_ready(surface, L"initial");
            if (success)
            {
                const HANDLE process = find_host();
                const auto before = cpu_ticks(process);
                const auto start = GetTickCount64();
                while (GetTickCount64() - start < 5000)
                {
                    const auto state = pump([&] { return surface.media_state(); });
                    if (!state || (state->flags & media_state_failed) != 0)
                    {
                        success = false;
                        break;
                    }
                    Sleep(20);
                }
                PROCESS_MEMORY_COUNTERS_EX memory{};
                if (process != nullptr && K32GetProcessMemoryInfo(process,
                    reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
                {
                    std::wcout << L"playback cpu_ms=" << (cpu_ticks(process) - before) / 10000
                               << L" private_mib=" << memory.PrivateUsage / (1024 * 1024)
                               << L" working_set_mib=" << memory.WorkingSetSize / (1024 * 1024)
                               << std::endl;
                }
                if (process != nullptr) CloseHandle(process);
                success = success && pump([&] { return surface.media_seek(20000000) &&
                    surface.media_set_view_mode(false); }) && wait_ready(surface, L"dual");
                success = success && pump([&] { return surface.media_set_view_mode(true); }) &&
                    wait_ready(surface, L"projected");
                success = success && pump([&] { return surface.media_set_view_mode(false) &&
                    surface.media_set_view_mode(true) && surface.media_set_view_mode(false); }) &&
                    wait_ready(surface, L"rapid_switch");
            }
            pump([&] { surface.shutdown(); });
            surface.destroy_surface();
        }
        DestroyWindow(parent);
        return success;
    }
}

int glance::tests::run_panorama_preview_benchmark(int argument_count, wchar_t* arguments[])
{
    if (argument_count < 3) return 2;
    std::wstring executable(32768, L'\0');
    executable.resize(GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())));
    const auto host = std::filesystem::path(executable).parent_path() /
        L"components" / L"panorama-video" / L"Glance.PanoramaVideoHost.exe";
    bool success = true;
    for (int index = 2; index < argument_count; ++index)
        success = measure(std::filesystem::absolute(arguments[index]).make_preferred().wstring(),
            host.wstring()) && success;
    return success ? 0 : 1;
}

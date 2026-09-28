#include "glance/contracts/dependency_runtime.h"
#include <array>
#include <iostream>

namespace
{
    std::filesystem::path executable_path()
    {
        std::wstring path(32768, L'\0');
        path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
        return path;
    }
}

int run_dependency_child(int count, wchar_t* arguments[])
{
    if (count < 3) return 9;
    const std::wstring_view mode(arguments[2]);
    if (mode == L"sleep") { Sleep(10000); return 0; }
    if (mode == L"exit") return 7;
    if (mode == L"arguments")
        return count == 6 && std::wstring_view(arguments[3]) == L"" &&
            std::wstring_view(arguments[4]) == L"space and \"quote\"\\" &&
            std::wstring_view(arguments[5]) == L"\u6587\u4ef6\\" ? 0 : 8;
    if (mode == L"streams")
    {
        const std::string out(4096, 'o'), err(4096, 'e');
        for (unsigned i = 0; i < 32; ++i)
        {
            DWORD written{};
            if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), out.data(), static_cast<DWORD>(out.size()), &written, nullptr)) return 10;
            if (!WriteFile(GetStdHandle(STD_ERROR_HANDLE), err.data(), static_cast<DWORD>(err.size()), &written, nullptr)) return 11;
        }
        return 0;
    }
    if (mode == L"descendant")
    {
        using glance::contracts::dependencies::quote_argument;
        const auto self = executable_path();
        auto command = quote_argument(self.wstring()) + L" --dependency-child sleep";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(self.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &process)) return 12;
        const auto pid = std::to_string(process.dwProcessId);
        DWORD written{};
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), pid.data(), static_cast<DWORD>(pid.size()), &written, nullptr);
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        return 0;
    }
    return 13;
}

int run_dependency_runtime_tests()
{
    using namespace glance::contracts::dependencies;
    unsigned failures{};
    const auto check = [&](bool value, const char* name) {
        std::cout << (value ? "PASS " : "FAIL ") << name << '\n'; failures += !value;
    };
    const auto self = executable_path();
    auto result = execute(self, {L"--dependency-child", L"arguments", L"", L"space and \"quote\"\\", L"\u6587\u4ef6\\"});
    check(result.succeeded(), "Dependency argv preserves empty, quoted, Unicode and trailing-slash arguments");
    result = execute(self, {L"--dependency-child", L"streams"});
    check(result.succeeded() && result.output == std::string(131072, 'o') && result.errors == std::string(131072, 'e'),
        "Dependency stdout and stderr drain separately beyond pipe capacity");
    result = execute(self, {L"--dependency-child", L"exit"});
    check(result.status == ExecutionStatus::completed && result.exit_code == 7, "Dependency nonzero exit preserved");
    result = execute(self, {L"--dependency-child", L"sleep"}, {.timeout_ms = 100});
    check(result.status == ExecutionStatus::timed_out, "Dependency timeout terminates process");
    const auto started = GetTickCount64();
    result = execute(self, {L"--dependency-child", L"sleep"}, {.cancelled = [started] { return GetTickCount64() - started >= 100; }});
    check(result.status == ExecutionStatus::cancelled, "Dependency cancellation terminates process");
    result = execute(self, {L"--dependency-child", L"streams"}, {.maximum_output_bytes = 1000});
    check(result.status == ExecutionStatus::output_limit && result.output.size() + result.errors.size() <= 1000,
        "Dependency output budget is enforced");
    result = execute(self, {L"--dependency-child", L"descendant"});
    bool child_stopped{};
    if (result.succeeded() && !result.output.empty())
    {
        const auto pid = static_cast<DWORD>(std::stoul(result.output));
        const auto child = OpenProcess(SYNCHRONIZE, FALSE, pid);
        child_stopped = child ? WaitForSingleObject(child, 1000) == WAIT_OBJECT_0 : GetLastError() == ERROR_INVALID_PARAMETER;
        if (child) CloseHandle(child);
    }
    check(child_stopped, "Dependency descendants do not survive completed invocation");
    check(execute(L"relative.exe", {}).error == ERROR_INVALID_PARAMETER, "Dependency relative executable rejected");
    check(execute(self.parent_path() / L"missing-dependency.exe", {}).status == ExecutionStatus::failed,
        "Missing dependency reports launch failure");
    std::array<wchar_t, MAX_PATH> system{};
    GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    Library library(std::filesystem::path(system.data()) / L"kernel32.dll");
    check(library.symbol("GetCurrentProcessId") != nullptr && library.symbol("GlanceMissingExport") == nullptr,
        "Dependency DLL export lookup");
    return failures ? 1 : 0;
}

#include "client.h"
#include <winrt/base.h>
namespace glance::executable
{
Result inspect(const std::wstring& host, const Query& query, const std::atomic_bool& cancelled)
{
    if (cancelled) winrt::throw_hresult(E_ABORT);
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    winrt::handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, sizeof(Transfer), nullptr));
    winrt::handle event(CreateEventW(&security, TRUE, FALSE, nullptr));
    if (!mapping || !event) winrt::throw_last_error();
    auto* memory = static_cast<Transfer*>(MapViewOfFile(mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Transfer)));
    if (!memory) winrt::throw_last_error();
    const auto unmap = [](Transfer* pointer) { UnmapViewOfFile(pointer); };
    std::unique_ptr<Transfer, decltype(unmap)> owner(memory, unmap);
    memory->query = query; memory->bytes = 0;
    std::array handles{mapping.get(), event.get()};
    SIZE_T size{}; InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<std::byte> attributes(size);
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size)) winrt::throw_last_error();
    const auto configured = UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(), sizeof(handles), nullptr, nullptr);
    auto command = L"\"" + host + L"\" --inspect " + std::to_wstring(reinterpret_cast<std::uintptr_t>(handles[0])) + L" " + std::to_wstring(reinterpret_cast<std::uintptr_t>(handles[1]));
    PROCESS_INFORMATION process{};
    const bool created = configured && CreateProcessW(host.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!created) winrt::throw_last_error();
    winrt::handle process_handle(process.hProcess), thread(process.hThread), job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limit.ProcessMemoryLimit = 512ULL * 1024 * 1024;
    if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limit, sizeof(limit)) ||
        !AssignProcessToJobObject(job.get(), process_handle.get()) || cancelled || ResumeThread(thread.get()) == DWORD(-1))
    { TerminateProcess(process_handle.get(), 1); winrt::throw_hresult(E_ABORT); }
    const auto started = GetTickCount64();
    while (WaitForSingleObject(process_handle.get(), 20) == WAIT_TIMEOUT)
    {
        if (cancelled || GetTickCount64() - started > 30000)
        {
            SetEvent(event.get());
            if (WaitForSingleObject(process_handle.get(), 100) == WAIT_TIMEOUT) TerminateJobObject(job.get(), 1);
            winrt::throw_hresult(cancelled ? E_ABORT : HRESULT_FROM_WIN32(WAIT_TIMEOUT));
        }
    }
    if (cancelled) winrt::throw_hresult(E_ABORT);
    DWORD code{}; GetExitCodeProcess(process_handle.get(), &code);
    if (code) winrt::throw_hresult(E_FAIL);
    return Reader(*memory).result();
}
}

#include "client.h"
#include <algorithm>
#include <array>

namespace glance::font
{
Client::~Client()
{
    cancel();
    if (pixels_)
        UnmapViewOfFile(pixels_);
}
void Client::cancel() noexcept
{
    cancelled_ = true;
}
void Client::open(const std::wstring &host, const std::wstring &path)
{
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    winrt::handle child_in, child_out;
    if (!CreatePipe(child_in.put(), output_.put(), &security, 128 * 1024) ||
        !CreatePipe(input_.put(), child_out.put(), &security, 128 * 1024))
        winrt::throw_last_error();
    SetHandleInformation(input_.get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(output_.get(), HANDLE_FLAG_INHERIT, 0);
    mapping_.attach(CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE | SEC_RESERVE, 0,
                                       64 * 1024 * 1024, nullptr));
    if (!mapping_)
        winrt::throw_last_error();
    pixels_ = MapViewOfFile(mapping_.get(), FILE_MAP_READ, 0, 0, 0);
    if (!pixels_)
        winrt::throw_last_error();
    std::array handles{child_in.get(), child_out.get(), mapping_.get()};
    SIZE_T size{};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<std::byte> attributes(size);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size))
        winrt::throw_last_error();
    const bool configured =
        UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  handles.data(), sizeof(handles), nullptr, nullptr) != FALSE;
    auto command = L"\"" + host + L"\" " + std::to_wstring(reinterpret_cast<std::uintptr_t>(handles[0])) +
                   L" " + std::to_wstring(reinterpret_cast<std::uintptr_t>(handles[1])) + L" " +
                   std::to_wstring(reinterpret_cast<std::uintptr_t>(handles[2]));
    PROCESS_INFORMATION process{};
    const bool created =
        configured && CreateProcessW(host.c_str(), command.data(), nullptr, nullptr, TRUE,
                                     CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                                     nullptr, nullptr, &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!created)
        winrt::throw_last_error();
    process_.attach(process.hProcess);
    winrt::handle thread(process.hThread);
    job_.attach(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
    limit.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limit.ProcessMemoryLimit = 512ULL * 1024 * 1024;
    if (!job_ ||
        !SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation, &limit, sizeof(limit)) ||
        !AssignProcessToJobObject(job_.get(), process_.get()) || ResumeThread(thread.get()) == DWORD(-1))
    {
        TerminateProcess(process_.get(), 1);
        winrt::throw_hresult(E_FAIL);
    }
    child_in.close();
    child_out.close();
    const auto length = static_cast<unsigned>(path.size());
    write(&length, sizeof(length));
    write(path.data(), length * 2);
}
void Client::write(const void *data, std::size_t bytes)
{
    if (cancelled_)
        winrt::throw_hresult(E_ABORT);
    DWORD written{};
    if (!WriteFile(output_.get(), data, static_cast<DWORD>(bytes), &written, nullptr) || written != bytes)
        winrt::throw_hresult(E_FAIL);
}
void Client::read(void *data, std::size_t bytes)
{
    const auto started = GetTickCount64();
    auto *target = static_cast<std::byte *>(data);
    while (bytes)
    {
        if (cancelled_ || GetTickCount64() - started > 10000)
        {
            job_.close();
            winrt::throw_hresult(cancelled_ ? E_ABORT : HRESULT_FROM_WIN32(WAIT_TIMEOUT));
        }
        DWORD available{};
        if (!PeekNamedPipe(input_.get(), nullptr, 0, nullptr, &available, nullptr))
            winrt::throw_hresult(E_FAIL);
        if (!available)
        {
            if (WaitForSingleObject(process_.get(), 10) == WAIT_OBJECT_0)
                winrt::throw_hresult(E_FAIL);
            continue;
        }
        DWORD count{};
        if (!ReadFile(input_.get(), target, static_cast<DWORD>(std::min<std::size_t>(available, bytes)),
                      &count, nullptr) ||
            !count)
            winrt::throw_hresult(E_FAIL);
        target += count;
        bytes -= count;
    }
}
Response Client::request(const Request &request)
{
    write(&request, sizeof(request));
    Response result;
    read(&result, sizeof(result));
    if (result.error)
        winrt::throw_hresult(E_FAIL);
    return result;
}
std::shared_ptr<Metadata> Client::metadata(unsigned face)
{
    Request query;
    query.operation = Operation::metadata;
    query.face = face;
    auto header = request(query);
    if (header.bytes != sizeof(Metadata))
        winrt::throw_hresult(E_FAIL);
    auto data = std::make_shared<Metadata>();
    read(data.get(), sizeof(*data));
    if (!data->count || data->count > maximum_faces || data->entry_count > std::size(data->entries))
        winrt::throw_hresult(E_FAIL);
    return data;
}
std::pair<Response, std::vector<std::byte>> Client::render(const Request &query)
{
    auto result = request(query);
    if (result.width != query.width || result.height != query.height ||
        result.bytes != std::uint64_t(result.width) * result.height * 4 || result.bytes > 64 * 1024 * 1024)
        winrt::throw_hresult(E_FAIL);
    std::vector<std::byte> data(result.bytes);
    memcpy(data.data(), pixels_, data.size());
    return {result, std::move(data)};
}
} // namespace glance::font

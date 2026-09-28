#include "glance/contracts/dependency_runtime.h"

#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace glance::contracts::dependencies
{
    namespace
    {
        struct CloseHandleDeleter
        {
            void operator()(void* handle) const noexcept
            { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
        };
        using Handle = std::unique_ptr<void, CloseHandleDeleter>;
        struct Pipe { Handle read, write; };

        void check(BOOL success)
        {
            if (!success) throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
        }

        Pipe make_pipe()
        {
            SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
            HANDLE read{}, write{};
            check(CreatePipe(&read, &write, &security, 0));
            Pipe result{Handle(read), Handle(write)};
            check(SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0));
            return result;
        }

        struct Attributes
        {
            std::vector<std::byte> storage;
            LPPROC_THREAD_ATTRIBUTE_LIST list{};
            explicit Attributes(std::array<HANDLE, 3>& handles)
            {
                SIZE_T bytes{};
                InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
                storage.resize(bytes);
                auto candidate = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
                check(InitializeProcThreadAttributeList(candidate, 1, 0, &bytes));
                if (!UpdateProcThreadAttribute(candidate, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                    handles.data(), sizeof(handles), nullptr, nullptr))
                {
                    const auto error = GetLastError();
                    DeleteProcThreadAttributeList(candidate);
                    throw std::system_error(static_cast<int>(error), std::system_category());
                }
                list = candidate;
            }
            ~Attributes() { if (list) DeleteProcThreadAttributeList(list); }
        };

        bool drain(HANDLE pipe, std::string& output, std::size_t& remaining)
        {
            // Bound each pass so a noisy stream cannot starve cancellation or stderr.
            std::array<char, 16384> bytes{};
            for (unsigned pass = 0; pass < 16; ++pass)
            {
                DWORD available{};
                if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
                {
                    if (GetLastError() == ERROR_BROKEN_PIPE) return true;
                    check(FALSE);
                }
                if (!available) return true;
                DWORD read{};
                check(ReadFile(pipe, bytes.data(), std::min<DWORD>(available, static_cast<DWORD>(bytes.size())), &read, nullptr));
                const auto keep = std::min<std::size_t>(read, remaining);
                output.append(bytes.data(), keep);
                remaining -= keep;
                if (read > keep) return false;
            }
            return true;
        }
    }

    std::wstring quote_argument(std::wstring_view value)
    {
        std::wstring result{L'"'};
        std::size_t slashes{};
        for (const wchar_t character : value)
        {
            if (character == L'\\') { ++slashes; continue; }
            result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
            result.push_back(character);
            slashes = 0;
        }
        result.append(slashes * 2, L'\\');
        result.push_back(L'"');
        return result;
    }

    ExecutionResult execute(const std::filesystem::path& executable,
        const std::vector<std::wstring>& arguments, const ExecutionOptions& options)
    {
        ExecutionResult result;
        try
        {
            if (!executable.is_absolute() || executable.native().find(L'\0') != std::wstring::npos ||
                std::ranges::any_of(arguments, [](const auto& value) { return value.find(L'\0') != std::wstring::npos; }) ||
                options.timeout_ms == 0 || options.maximum_output_bytes == 0)
            {
                result.error = ERROR_INVALID_PARAMETER;
                return result;
            }
            if (options.cancelled && options.cancelled())
            {
                result.status = ExecutionStatus::cancelled;
                return result;
            }
            auto output = make_pipe();
            auto errors = make_pipe();
            SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
            Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            check(input.get() != INVALID_HANDLE_VALUE);
            std::array<HANDLE, 3> handles{input.get(), output.write.get(), errors.write.get()};
            Attributes attributes(handles);
            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput = input.get();
            startup.StartupInfo.hStdOutput = output.write.get();
            startup.StartupInfo.hStdError = errors.write.get();
            startup.lpAttributeList = attributes.list;
            Handle job(CreateJobObjectW(nullptr, nullptr));
            check(job.get() != nullptr);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            check(SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)));
            auto command = quote_argument(executable.wstring());
            for (const auto& argument : arguments) { command += L' '; command += quote_argument(argument); }
            if (command.size() >= 32767) { result.error = ERROR_BAD_LENGTH; return result; }
            PROCESS_INFORMATION process{};
            const auto working_directory = executable.parent_path().wstring();
            check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                working_directory.c_str(), &startup.StartupInfo, &process));
            Handle process_handle(process.hProcess), thread(process.hThread);
            if (!AssignProcessToJobObject(job.get(), process.hProcess))
            {
                const auto error = GetLastError();
                TerminateProcess(process.hProcess, error);
                WaitForSingleObject(process.hProcess, 1000);
                throw std::system_error(static_cast<int>(error), std::system_category());
            }
            check(ResumeThread(thread.get()) != static_cast<DWORD>(-1));
            output.write.reset(); errors.write.reset(); input.reset();
            const auto started = GetTickCount64();
            auto remaining = options.maximum_output_bytes;
            for (;;)
            {
                if (!drain(output.read.get(), result.output, remaining) || !drain(errors.read.get(), result.errors, remaining))
                { result.status = ExecutionStatus::output_limit; break; }
                if (options.cancelled && options.cancelled())
                { result.status = ExecutionStatus::cancelled; break; }
                const auto wait = WaitForSingleObject(process_handle.get(), 10);
                if (wait == WAIT_OBJECT_0)
                {
                    check(GetExitCodeProcess(process_handle.get(), &result.exit_code));
                    // Descendants must not outlive a completed invocation or keep pipes open.
                    check(TerminateJobObject(job.get(), ERROR_PROCESS_ABORTED));
                    result.status = ExecutionStatus::completed;
                    for (;;)
                    {
                        const auto before = remaining;
                        if (!drain(output.read.get(), result.output, remaining) || !drain(errors.read.get(), result.errors, remaining))
                        { result.status = ExecutionStatus::output_limit; break; }
                        if (before == remaining) break;
                    }
                    return result;
                }
                if (wait == WAIT_FAILED) check(FALSE);
                if (GetTickCount64() - started >= options.timeout_ms)
                { result.status = ExecutionStatus::timed_out; break; }
            }
            TerminateJobObject(job.get(), ERROR_PROCESS_ABORTED);
            WaitForSingleObject(process_handle.get(), 1000);
            GetExitCodeProcess(process_handle.get(), &result.exit_code);
        }
        catch (const std::system_error& error)
        { result.error = static_cast<DWORD>(error.code().value()); result.status = ExecutionStatus::failed; }
        return result;
    }

    Library::Library(const std::filesystem::path& path)
    {
        if (!path.is_absolute()) throw std::invalid_argument("Library path must be absolute");
        module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        check(module_ != nullptr);
    }
    Library::~Library() { if (module_) FreeLibrary(module_); }
    Library::Library(Library&& other) noexcept : module_(std::exchange(other.module_, nullptr)) {}
    Library& Library::operator=(Library&& other) noexcept
    {
        if (this != &other)
        {
            if (module_) FreeLibrary(module_);
            module_ = std::exchange(other.module_, nullptr);
        }
        return *this;
    }
    FARPROC Library::symbol(const char* name) const noexcept
    { return module_ && name ? GetProcAddress(module_, name) : nullptr; }
}

#include "glance/contracts/maintenance.h"
#include "glance/contracts/cli_protocol.h"
#include <bcrypt.h>
#include <algorithm>
#include <fstream>
#include <cstring>
#include <cwctype>
#include <set>
#include <system_error>
#include <rapidjson/document.h>

namespace
{
    using namespace glance::contracts;
    using Path = std::filesystem::path;
    using glance::cli::Handle;
    constexpr wchar_t workspace_name[] = L".glance-maintenance";

    [[noreturn]] void fail(DWORD error)
    {
        throw std::system_error(error, std::system_category());
    }
    void check(LSTATUS status)
    {
        if (status != ERROR_SUCCESS)
            fail(status);
    }
    Path workspace(const Path &application)
    {
        return application / workspace_name;
    }
    void safe_path(const Path &path);
    bool process_alive(DWORD id)
    {
        Handle process(OpenProcess(SYNCHRONIZE, FALSE, id));
        if (!process.value)
        {
            if (GetLastError() == ERROR_INVALID_PARAMETER)
                return false;
            fail(GetLastError());
        }
        return WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT;
    }
    void clean_helpers()
    {
        const auto root = std::filesystem::temp_directory_path() / L"Glance";
        if (!std::filesystem::is_directory(root))
            return;
        for (const auto &entry : std::filesystem::directory_iterator(root))
        {
            if (!entry.is_directory() || !entry.path().filename().wstring().starts_with(L"Maintenance-"))
                continue;
            try
            {
                safe_path(entry.path());
                std::ifstream owner(entry.path() / L"owner");
                DWORD id{};
                if (!(owner >> id) || !id || process_alive(id))
                    continue;
                owner.close();
                std::filesystem::remove_all(entry.path());
            }
            catch (...)
            {
            }
        }
    }
    void safe_path(const Path &path)
    {
        for (auto current = std::filesystem::absolute(path); !current.empty(); current = current.parent_path())
        {
            const auto attributes = GetFileAttributesW(current.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
                fail(ERROR_REPARSE_TAG_INVALID);
            if (current == current.parent_path())
                break;
        }
    }
    std::wstring quote(std::wstring_view value)
    {
        std::wstring result = L"\"";
        std::size_t slashes{};
        for (const auto c : value)
        {
            if (c == L'\\')
            {
                ++slashes;
                continue;
            }
            result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
            slashes = 0;
            if (c == L'"')
                result += L'\\';
            result += c;
        }
        result.append(slashes * 2, L'\\');
        return result + L'"';
    }
    DWORD execute(const Path &executable, std::wstring arguments, DWORD timeout = 120000)
    {
        Handle job(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job.value ||
            !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            fail(GetLastError());
        auto command = quote(executable.wstring()) + L" " + arguments;
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, executable.parent_path().c_str(), &startup,
                            &process))
            fail(GetLastError());
        Handle owner(process.hProcess), thread(process.hThread);
        if (!AssignProcessToJobObject(job.value, owner.value) || ResumeThread(thread.value) == static_cast<DWORD>(-1))
        {
            const auto error = GetLastError();
            TerminateProcess(owner.value, error);
            fail(error);
        }
        if (WaitForSingleObject(owner.value, timeout) != WAIT_OBJECT_0)
        {
            TerminateProcess(owner.value, ERROR_TIMEOUT);
            WaitForSingleObject(owner.value, 5000);
            fail(ERROR_TIMEOUT);
        }
        DWORD result{};
        if (!GetExitCodeProcess(owner.value, &result))
            fail(GetLastError());
        return result;
    }
    std::wstring hash_file(const Path &file)
    {
        Handle handle(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (handle.value == INVALID_HANDLE_VALUE)
            fail(GetLastError());
        BCRYPT_ALG_HANDLE algorithm{};
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            fail(ERROR_GEN_FAILURE);
        struct Algorithm
        {
            BCRYPT_ALG_HANDLE value;
            ~Algorithm()
            {
                BCryptCloseAlgorithmProvider(value, 0);
            }
        } algorithm_owner{algorithm};
        BCRYPT_HASH_HANDLE hash{};
        if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
            fail(ERROR_GEN_FAILURE);
        struct Hash
        {
            BCRYPT_HASH_HANDLE value;
            ~Hash()
            {
                BCryptDestroyHash(value);
            }
        } hash_owner{hash};
        std::vector<std::uint8_t> buffer(256 * 1024);
        DWORD read{};
        for (;;)
        {
            if (!ReadFile(handle.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
                fail(GetLastError());
            if (!read)
                break;
            if (BCryptHashData(hash, buffer.data(), read, 0) < 0)
                fail(ERROR_GEN_FAILURE);
        }
        std::uint8_t digest[32]{};
        if (BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0)
            fail(ERROR_GEN_FAILURE);
        constexpr wchar_t digits[] = L"0123456789abcdef";
        std::wstring result;
        for (auto b : digest)
        {
            result += digits[b >> 4];
            result += digits[b & 15];
        }
        return result;
    }
    void put(storage::Snapshot &state, std::wstring_view group, const std::wstring &name, const std::wstring &value)
    {
        const auto bytes = reinterpret_cast<const std::uint8_t *>(value.c_str());
        state[std::wstring(group)][name] = {REG_SZ, {bytes, bytes + (value.size() + 1) * sizeof(wchar_t)}};
    }
    std::wstring get(const storage::Snapshot &state, const std::wstring &group, const std::wstring &name)
    {
        const auto &value = state.at(group).at(name);
        if (value.type != REG_SZ || value.bytes.size() < sizeof(wchar_t) || value.bytes.size() % sizeof(wchar_t))
            fail(ERROR_INVALID_DATA);
        std::wstring result(value.bytes.size() / sizeof(wchar_t), L'\0');
        std::memcpy(result.data(), value.bytes.data(), value.bytes.size());
        if (result.back() != L'\0')
            fail(ERROR_INVALID_DATA);
        result.pop_back();
        return result;
    }
    storage::Snapshot inventory(const Path &root)
    {
        storage::Snapshot result;
        safe_path(root);
        if (!std::filesystem::exists(root))
            return result;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(root))
        {
            safe_path(entry.path());
            const auto relative = std::filesystem::relative(entry.path(), root).wstring();
            if (entry.is_directory())
                put(result, L"Directories", relative, L"");
            else if (entry.is_regular_file())
                put(result, L"Files", relative,
                    hash_file(entry.path()) + L":" + std::to_wstring(entry.file_size()) + L":" +
                        std::to_wstring(entry.last_write_time().time_since_epoch().count()));
            else
                fail(ERROR_INVALID_DATA);
        }
        return result;
    }
    void copy_tree(const Path &source, const Path &target)
    {
        safe_path(source);
        safe_path(target);
        std::filesystem::create_directories(target);
        if (!std::filesystem::exists(source))
            return;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(source))
        {
            safe_path(entry.path());
            const auto destination = target / std::filesystem::relative(entry.path(), source);
            if (entry.is_directory())
                std::filesystem::create_directories(destination);
            else if (entry.is_regular_file())
            {
                std::filesystem::create_directories(destination.parent_path());
                std::filesystem::copy_file(entry.path(), destination);
                std::filesystem::last_write_time(destination, entry.last_write_time());
            }
            else
                fail(ERROR_INVALID_DATA);
        }
    }
    void move(const Path &source, const Path &target, bool replace = false)
    {
        std::filesystem::create_directories(target.parent_path());
        if (!MoveFileExW(source.c_str(), target.c_str(),
                         MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0)))
            fail(GetLastError());
    }
    storage::Snapshot journal(const Path &root)
    {
        storage::Snapshot result;
        check(storage::read_snapshot(workspace(root) / L"journal.json", result));
        return result;
    }
    void save(const Path &root, const storage::Snapshot &state)
    {
        check(storage::write_snapshot(workspace(root) / L"journal.json", state));
    }
    void migration_warning(const storage::Snapshot &state, const wchar_t *message_key, DWORD error) noexcept
    {
        try
        {
            const auto &fields = state.at(L"Job");
            const auto title = fields.contains(L"warning-title") ? get(state, L"Job", L"warning-title") : L"Glance";
            auto message = fields.contains(message_key) ? get(state, L"Job", message_key) : std::wstring{};
            wchar_t details[2048]{};
            if (FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
                               details, ARRAYSIZE(details), nullptr))
                message += L"\n" + std::wstring(details);
            MessageBoxW(nullptr, message.c_str(), title.c_str(), MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        }
        catch (...) {}
    }
    void validate_relative(const Path &path)
    {
        if (path.empty() || path.is_absolute() || path.has_root_name())
            fail(ERROR_INVALID_NAME);
        for (const auto &part : path)
            if (part == L".." || part == L"." || part.wstring().find(L':') != std::wstring::npos)
                fail(ERROR_INVALID_NAME);
        const auto first = path.begin()->wstring();
        if (CompareStringOrdinal(first.c_str(), -1, workspace_name, -1, TRUE) == CSTR_EQUAL ||
            CompareStringOrdinal(first.c_str(), -1, L"data", -1, TRUE) == CSTR_EQUAL)
            fail(ERROR_INVALID_NAME);
    }
    void rollback(const Path &root, storage::Snapshot &state)
    {
        const bool migration = get(state, L"Job", L"kind") == L"migration";
        for (const auto &[name, unused] : state.at(L"Targets"))
        {
            static_cast<void>(unused);
            const Path relative(name);
            if (!(migration && relative == L"data"))
                validate_relative(relative);
            const auto target = root / relative;
            const auto backup = workspace(root) / L"backup" / relative;
            const auto staged = workspace(root) / L"stage" / relative;
            safe_path(target);
            safe_path(backup);
            if (std::filesystem::exists(backup) && (migration || !std::filesystem::exists(staged)))
            {
                if (migration)
                    std::filesystem::remove_all(target);
                move(backup, target, !migration);
            }
            else if (get(state, L"Targets", name) == L"new" && !std::filesystem::exists(staged))
                std::filesystem::remove_all(target);
        }
        put(state, L"Job", L"state", L"rolled-back");
        save(root, state);
    }
    HANDLE restart(const Path &root, bool maintenance, storage::Snapshot *state = nullptr)
    {
        const auto executable = root / L"Glance.exe";
        auto command = quote(executable.wstring()) + (maintenance ? L" --maintenance" : L"");
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                            root.c_str(), &startup, &process))
            fail(GetLastError());
        Handle thread(process.hThread);
        try
        {
            if (state)
            {
                put(*state, L"Job", L"startup", std::to_wstring(process.dwProcessId));
                save(root, *state);
            }
            if (ResumeThread(thread.value) == static_cast<DWORD>(-1))
                fail(GetLastError());
        }
        catch (...)
        {
            TerminateProcess(process.hProcess, ERROR_PROCESS_ABORTED);
            CloseHandle(process.hProcess);
            throw;
        }
        return process.hProcess;
    }
    void stop_restarted(HANDLE process)
    {
        Handle shutdown(OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\Glance.Shutdown"));
        if (shutdown.value)
            SetEvent(shutdown.value);
        if (WaitForSingleObject(process, 10000) != WAIT_OBJECT_0)
        {
            if (!TerminateProcess(process, ERROR_PROCESS_ABORTED) ||
                WaitForSingleObject(process, 5000) != WAIT_OBJECT_0)
                fail(GetLastError());
        }
        const auto deadline = GetTickCount64() + 10000;
        while (GetTickCount64() < deadline)
        {
            Handle core(OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\Glance.Core"));
            if (!core.value)
                return;
            Sleep(50);
        }
        fail(ERROR_TIMEOUT);
    }
    bool ready(DWORD expected_process) noexcept
    {
        try
        {
            Handle pipe(CreateFileW(glance::cli::pipe_name().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
            ULONG server{};
            if (pipe.value == INVALID_HANDLE_VALUE || !GetNamedPipeServerProcessId(pipe.value, &server) ||
                server != expected_process)
                return false;
            const auto deadline = GetTickCount64() + 1500;
            if (!glance::cli::send(pipe.value, R"({"command":"status","timeout_ms":1000})", 1, deadline, nullptr))
                return false;
            glance::cli::Header header;
            const auto response = glance::cli::receive(pipe.value, header, deadline, nullptr);
            rapidjson::Document result;
            result.Parse(response.data(), response.size());
            if (result.HasParseError() || !result.IsObject() || !result.HasMember("ok") || !result["ok"].IsBool() ||
                !result["ok"].GetBool() || !result.HasMember("data") || !result["data"].IsObject())
                return false;
            const auto &data = result["data"];
            return data.HasMember("process_id") && data["process_id"].IsNumber() &&
                   data["process_id"].GetDouble() == expected_process && data.HasMember("core_connected") &&
                   data["core_connected"].IsBool() && data["core_connected"].GetBool();
        }
        catch (...)
        {
            return false;
        }
    }
    void begin(const Path &root, std::wstring_view kind)
    {
        safe_path(root);
        maintenance::recover(root);
        if (std::filesystem::exists(workspace(root)))
            fail(ERROR_BUSY);
        try
        {
            std::filesystem::create_directories(workspace(root) / L"stage");
            storage::Snapshot state;
            put(state, L"Job", L"kind", std::wstring(kind));
            put(state, L"Job", L"state", L"preparing");
            save(root, state);
        }
        catch (...)
        {
            std::error_code ignored;
            std::filesystem::remove_all(workspace(root), ignored);
            throw;
        }
    }
} // namespace

namespace glance::contracts::maintenance
{
    bool installed_data_available() noexcept
    {
        try
        {
            storage::Snapshot settings;
            storage::SettingsStore source(storage::Backend::registry);
            check(source.snapshot(settings));
            return !settings.empty() || (std::filesystem::exists(storage::installed_data_directory()) &&
                                         !std::filesystem::is_empty(storage::installed_data_directory()));
        }
        catch (...)
        {
            return false;
        }
    }
    void prepare_migration(const Path &application)
    {
        begin(application, L"migration");
        try
        {
            if (!installed_data_available())
                fail(ERROR_FILE_NOT_FOUND);
            const auto source = std::filesystem::weakly_canonical(storage::installed_data_directory());
            const auto target = std::filesystem::weakly_canonical(application / L"data");
            auto a = source.wstring(), b = target.wstring();
            std::ranges::transform(a, a.begin(), towlower);
            std::ranges::transform(b, b.begin(), towlower);
            if (a == b || a.starts_with(b + L"\\") || b.starts_with(a + L"\\"))
                fail(ERROR_INVALID_PARAMETER);
            storage::SettingsStore source_settings(storage::Backend::registry);
            storage::Snapshot settings;
            check(source_settings.snapshot(settings));
            const auto before = inventory(source);
            const auto staged = workspace(application) / L"stage" / L"data";
            copy_tree(source, staged);
            if (before != inventory(source) || before != inventory(staged))
                fail(ERROR_RETRY);
            check(storage::write_snapshot(workspace(application) / L"source-settings.json", settings));
            check(storage::write_snapshot(workspace(application) / L"source-files.json", before));
            check(storage::write_snapshot(staged / L"settings.json", settings));
            storage::Snapshot verified, current;
            check(storage::read_snapshot(staged / L"settings.json", verified));
            check(source_settings.snapshot(current));
            if (settings != verified || settings != current)
                fail(ERROR_RETRY);
            auto state = journal(application);
            put(state, L"Job", L"source", source.wstring());
            put(state, L"Targets", L"data", std::filesystem::exists(target) ? L"existing" : L"new");
            put(state, L"Job", L"state", L"prepared");
            save(application, state);
        }
        catch (...)
        {
            std::filesystem::remove_all(workspace(application));
            throw;
        }
    }
    void prepare_update(const Path &application, const Path &archive, std::wstring_view version)
    {
        if (version.empty() || version.size() > 64 || !std::ranges::all_of(version, [](wchar_t value) {
                return (value >= L'0' && value <= L'9') || value == L'.';
            }))
            fail(ERROR_INVALID_PARAMETER);
        safe_path(archive);
        begin(application, L"update");
        try
        {
            const auto extraction = workspace(application) / L"unpacked";
            const auto script = workspace(application) / L"extract.ps1";
            std::ofstream out(script);
            out << R"PS(param([string]$Archive, [string]$Destination)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($Archive)
try {
    $root = [IO.Path]::GetFullPath($Destination).TrimEnd('\') + '\'
    $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    [long]$total = 0
    if ($zip.Entries.Count -gt 50000) { throw 'Too many entries' }
    foreach ($entry in $zip.Entries) {
        $name = $entry.FullName.Replace('/', '\')
        if ([IO.Path]::IsPathRooted($name) -or $name.Contains(':') -or $name -match '(^|\\)\.\.?($|\\)' -or $name -match '[\x00-\x1f]' -or (($entry.ExternalAttributes -shr 16) -band 0xf000) -eq 0xa000 -or ($entry.ExternalAttributes -band 0x400)) { throw 'Unsafe entry' }
        $path = [IO.Path]::GetFullPath($root + $name)
        if (!$path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) -or !$names.Add($path.TrimEnd('\'))) { throw 'Unsafe or duplicate path' }
        foreach ($part in $name.TrimEnd('\').Split('\')) { if ($part -match '[\. ]$|^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\.|$)') { throw 'Invalid name' } }
        $total += $entry.Length
        if ($total -gt 8589934592) { throw 'Archive too large' }
    }
    foreach ($entry in $zip.Entries) {
        $path = [IO.Path]::GetFullPath($root + $entry.FullName.Replace('/', '\'))
        if ($entry.FullName.EndsWith('/')) { [IO.Directory]::CreateDirectory($path) | Out-Null }
        else {
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $path, $false)
        }
    }
} finally { $zip.Dispose() }
)PS";
            out.close();
            wchar_t system[32768]{};
            if (!GetSystemDirectoryW(system, ARRAYSIZE(system)))
                fail(GetLastError());
            const auto powershell = Path(system) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
            check(execute(powershell, L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -File " +
                                          quote(script.wstring()) + L" -Archive " + quote(archive.wstring()) +
                                          L" -Destination " + quote(extraction.wstring())));
            const auto payload = extraction / (L"Glance-" + std::wstring(version) + L"-x64");
            safe_path(payload);
            if (!std::filesystem::is_directory(payload))
                fail(ERROR_INVALID_DATA);
            for (const auto name : {L"Glance.exe", L"Glance.Core.exe", L"Glance.CLI.exe", L"Glance.pri", L"App.xbf",
                                    L"MainWindow.xbf", L"SettingsWindow.xbf", L"Scintilla.dll", L"Lexilla.dll"})
                if (!std::filesystem::is_regular_file(payload / name))
                    fail(ERROR_FILE_NOT_FOUND);
            for (const auto name : {L"Glance.exe", L"Glance.Core.exe", L"Glance.CLI.exe"})
                if (!storage::binary_matches(payload / name, true, version))
                    fail(ERROR_INVALID_DATA);
            const auto files = inventory(payload);
            auto state = journal(application);
            for (const auto &[name, value] : files.at(L"Files"))
            {
                static_cast<void>(value);
                validate_relative(Path(name));
                put(state, L"Targets", name, std::filesystem::exists(application / name) ? L"existing" : L"new");
            }
            copy_tree(payload, workspace(application) / L"stage");
            if (files != inventory(workspace(application) / L"stage"))
                fail(ERROR_CRC);
            put(state, L"Job", L"state", L"prepared");
            save(application, state);
        }
        catch (...)
        {
            std::filesystem::remove_all(workspace(application));
            throw;
        }
    }
    void launch_worker(const Path &application, const MigrationOptions &options)
    {
        auto state = journal(application);
        const auto phase = get(state, L"Job", L"state");
        if (phase != L"prepared" && phase != L"committing" && phase != L"starting")
            fail(ERROR_INVALID_STATE);
        if (phase == L"prepared" && get(state, L"Job", L"kind") == L"migration")
        {
            put(state, L"Job", L"remove-source", options.remove_source ? L"yes" : L"no");
            put(state, L"Job", L"warning-title", options.warning_title);
            put(state, L"Job", L"cleanup-failure", options.cleanup_failure_message);
            put(state, L"Job", L"restart-failure", options.restart_failure_message);
        }
        const auto temp =
            std::filesystem::temp_directory_path() / L"Glance" /
            (L"Maintenance-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(temp);
        const auto helper = temp / L"Glance.CLI.exe";
        std::filesystem::copy_file(application / L"Glance.CLI.exe", helper);
        auto command = quote(helper.wstring()) + L" --internal-maintenance " + quote(application.wstring()) + L" " +
                       std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, temp.c_str(), &startup, &process))
        {
            const auto error = GetLastError();
            std::filesystem::remove_all(temp);
            fail(error);
        }
        Handle thread(process.hThread), owner(process.hProcess);
        try
        {
            std::ofstream marker(temp / L"owner");
            marker << process.dwProcessId;
            marker.close();
            if (!marker)
                fail(ERROR_WRITE_FAULT);
            put(state, L"Job", L"worker", std::to_wstring(process.dwProcessId));
            save(application, state);
            if (ResumeThread(thread.value) == static_cast<DWORD>(-1))
                fail(GetLastError());
        }
        catch (...)
        {
            TerminateProcess(owner.value, ERROR_PROCESS_ABORTED);
            WaitForSingleObject(owner.value, 5000);
            std::error_code ignored;
            std::filesystem::remove_all(temp, ignored);
            throw;
        }
    }
    void recover(const Path &application)
    {
        clean_helpers();
        if (!std::filesystem::exists(workspace(application)))
            return;
        safe_path(workspace(application));
        if (!std::filesystem::exists(workspace(application) / L"journal.json"))
            fail(ERROR_INVALID_DATA);
        auto state = journal(application);
        const auto phase = get(state, L"Job", L"state");
        if (state.at(L"Job").contains(L"worker") &&
            process_alive(static_cast<DWORD>(std::stoul(get(state, L"Job", L"worker")))))
        {
            if (phase == L"starting" && state.at(L"Job").contains(L"startup") &&
                std::stoul(get(state, L"Job", L"startup")) == GetCurrentProcessId())
                return;
            if (phase != L"complete" && phase != L"rolled-back")
                fail(ERROR_BUSY);
        }
        if (phase == L"committing" || phase == L"starting")
        {
            rollback(application, state);
            put(state, L"Job", L"error", std::to_wstring(ERROR_PROCESS_ABORTED));
            save(application, state);
            return;
        }
        else if (phase == L"rolled-back" && state.at(L"Job").contains(L"error"))
            return;
        std::filesystem::remove_all(workspace(application));
    }
    bool recovery_required(const Path &application)
    {
        if (!std::filesystem::exists(workspace(application)))
            return false;
        const auto state = journal(application);
        const auto phase = get(state, L"Job", L"state");
        return (phase == L"committing" || phase == L"starting") &&
               (!state.at(L"Job").contains(L"worker") ||
                !process_alive(static_cast<DWORD>(std::stoul(get(state, L"Job", L"worker")))));
    }
    int run_worker(const Path &application, DWORD parent) noexcept
    {
        bool parent_exited{};
        Handle restarted;
        Handle job(CreateJobObjectW(nullptr, nullptr));
        const auto release_children = [&] {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            if (job.value)
                SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        };
        try
        {
            auto state = journal(application);
            const bool recovery = get(state, L"Job", L"state") != L"prepared";
            Handle process(OpenProcess(SYNCHRONIZE, FALSE, parent));
            if (!process.value && GetLastError() != ERROR_INVALID_PARAMETER)
                fail(GetLastError());
            if (process.value && process.value != INVALID_HANDLE_VALUE &&
                WaitForSingleObject(process.value, 30000) != WAIT_OBJECT_0)
                fail(ERROR_TIMEOUT);
            parent_exited = true;
            if (recovery)
            {
                Handle shutdown(OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\Glance.Shutdown"));
                if (shutdown.value)
                    SetEvent(shutdown.value);
            }
            const auto deadline = GetTickCount64() + 30000;
            while (GetTickCount64() < deadline)
            {
                Handle core(OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\Glance.Core"));
                Handle app(OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\Glance.App"));
                if (!core.value && !app.value)
                    break;
                Sleep(50);
            }
            Handle remaining_core(OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\Glance.Core"));
            Handle remaining_app(OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\Glance.App"));
            if (remaining_core.value || remaining_app.value)
                fail(ERROR_TIMEOUT);
            safe_path(application);
            state = journal(application);
            if (recovery)
            {
                rollback(application, state);
                put(state, L"Job", L"error", std::to_wstring(ERROR_PROCESS_ABORTED));
                save(application, state);
                Handle original(restart(application, true));
                return 0;
            }
            if (get(state, L"Job", L"state") != L"prepared")
                fail(ERROR_INVALID_STATE);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!job.value ||
                !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
                !AssignProcessToJobObject(job.value, GetCurrentProcess()))
                fail(GetLastError());
            put(state, L"Job", L"state", L"committing");
            save(application, state);
            try
            {
                for (const auto &[name, unused] : state.at(L"Targets"))
                {
                    static_cast<void>(unused);
                    const Path relative(name);
                    if (!(get(state, L"Job", L"kind") == L"migration" && relative == L"data"))
                        validate_relative(relative);
                    const auto target = application / relative;
                    safe_path(target);
                    const bool migration = get(state, L"Job", L"kind") == L"migration";
                    if (std::filesystem::exists(target))
                    {
                        const auto backup = workspace(application) / L"backup" / relative;
                        if (migration)
                            move(target, backup);
                        else
                        {
                            const auto copying = workspace(application) / L"copying" / relative;
                            std::filesystem::create_directories(copying.parent_path());
                            std::filesystem::create_directories(backup.parent_path());
                            std::filesystem::copy_file(target, copying);
                            move(copying, backup);
                        }
                    }
                    move(workspace(application) / L"stage" / relative, target, !migration);
                }
                put(state, L"Job", L"state", L"starting");
                put(state, L"Job", L"worker", std::to_wstring(GetCurrentProcessId()));
                save(application, state);
            }
            catch (...)
            {
                rollback(application, state);
                throw;
            }
            if (get(state, L"Job", L"kind") == L"migration")
            {
                put(state, L"Job", L"state", L"complete");
                save(application, state);
                if (state.at(L"Job").contains(L"remove-source") && get(state, L"Job", L"remove-source") == L"yes")
                {
                    try { delete_migration_source(application); }
                    catch (const std::system_error &error)
                    {
                        migration_warning(state, L"cleanup-failure", static_cast<DWORD>(error.code().value()));
                    }
                    catch (...) { migration_warning(state, L"cleanup-failure", ERROR_GEN_FAILURE); }
                }
                std::error_code ignored;
                std::filesystem::remove_all(workspace(application), ignored);
                release_children();
                try { Handle migrated(restart(application, false)); }
                catch (const std::system_error &error)
                {
                    migration_warning(state, L"restart-failure", static_cast<DWORD>(error.code().value()));
                    return static_cast<int>(error.code().value());
                }
                catch (...)
                {
                    migration_warning(state, L"restart-failure", ERROR_GEN_FAILURE);
                    return ERROR_GEN_FAILURE;
                }
                return 0;
            }
            restarted.value = restart(application, false, &state);
            bool loaded{};
            const auto ready_deadline = GetTickCount64() + 20000;
            while (GetTickCount64() < ready_deadline && WaitForSingleObject(restarted.value, 0) == WAIT_TIMEOUT)
            {
                if (ready(GetProcessId(restarted.value)))
                {
                    loaded = true;
                    break;
                }
                Sleep(100);
            }
            if (!loaded)
            {
                stop_restarted(restarted.value);
                rollback(application, state);
                fail(ERROR_PROCESS_ABORTED);
            }
            put(state, L"Job", L"state", L"complete");
            save(application, state);
            release_children();
            if (get(state, L"Job", L"kind") == L"update")
            {
                std::error_code ignored;
                std::filesystem::remove_all(workspace(application), ignored);
            }
            return 0;
        }
        catch (...)
        {
            DWORD error = ERROR_GEN_FAILURE;
            try
            {
                throw;
            }
            catch (const std::system_error &failure)
            {
                error = static_cast<DWORD>(failure.code().value());
            }
            catch (...)
            {
            }
            try
            {
                auto state = journal(application);
                if (restarted.value != INVALID_HANDLE_VALUE)
                    stop_restarted(restarted.value);
                const auto phase = get(state, L"Job", L"state");
                if (phase == L"committing" || phase == L"starting")
                    rollback(application, state);
                if (get(state, L"Job", L"state") != L"committing" && get(state, L"Job", L"state") != L"starting")
                    put(state, L"Job", L"state", L"rolled-back");
                put(state, L"Job", L"error", std::to_wstring(error));
                save(application, state);
                release_children();
                if (parent_exited)
                {
                    Handle original(restart(application, true));
                }
            }
            catch (...)
            {
            }
            return static_cast<int>(error);
        }
    }
    DWORD failure(const Path &application)
    {
        if (!std::filesystem::exists(workspace(application)))
            return ERROR_SUCCESS;
        const auto state = journal(application);
        if (!state.at(L"Job").contains(L"error"))
            return ERROR_SUCCESS;
        return static_cast<DWORD>(std::stoul(get(state, L"Job", L"error")));
    }
    void delete_migration_source(const Path &application)
    {
        auto state = journal(application);
        if (get(state, L"Job", L"state") != L"complete" || get(state, L"Job", L"kind") != L"migration")
            fail(ERROR_INVALID_STATE);
        const Path source(get(state, L"Job", L"source"));
        if (source != std::filesystem::weakly_canonical(storage::installed_data_directory()))
            fail(ERROR_INVALID_DATA);
        storage::Snapshot old_settings, old_files, current;
        check(storage::read_snapshot(workspace(application) / L"source-settings.json", old_settings));
        check(storage::read_snapshot(workspace(application) / L"source-files.json", old_files));
        storage::SettingsStore source_settings(storage::Backend::registry);
        check(source_settings.snapshot(current));
        if (old_settings != current || old_files != inventory(source))
            fail(ERROR_RETRY);
        // Move the verified source out of its live location before deleting it.
        const auto retired = source.parent_path() / (L"Glance-migrated-" + std::to_wstring(GetTickCount64()));
        if (std::filesystem::exists(source))
            move(source, retired);
        try
        {
            check(source_settings.snapshot(current));
            if (old_settings != current || old_files != inventory(retired))
                fail(ERROR_RETRY);
            check(source_settings.clear(L""));
        }
        catch (...)
        {
            if (std::filesystem::exists(retired))
                move(retired, source);
            throw;
        }
        if (std::filesystem::exists(retired))
            std::filesystem::remove_all(retired);
    }
    void finish_migration(const Path &application)
    {
        std::filesystem::remove_all(workspace(application));
    }
} // namespace glance::contracts::maintenance

#include "glance/contracts/cli_protocol.h"
#include "glance/contracts/cli_input.h"
#include "../version.h"
#include "help.h"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace
{
    using namespace glance::cli;
    using Json = rapidjson::Document;
    HANDLE interrupted{};
    HANDLE main_thread{};
    bool quiet_output{};
    BOOL WINAPI interrupt(DWORD event)
    {
        if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
        SetEvent(interrupted);
        if (main_thread) CancelSynchronousIo(main_thread);
        return TRUE;
    }
    std::string utf8(std::wstring_view value)
    {
        const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (!size && !value.empty()) throw Error(2, "invalid_unicode", "Invalid Unicode argument");
        std::string result(size, '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
        return result;
    }
    std::wstring wide(std::string_view value)
    {
        const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
        std::wstring result(size, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
        return result;
    }
    std::string serialize(const rapidjson::Value& value)
    {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        value.Accept(writer);
        return { buffer.GetString(), buffer.GetSize() };
    }
    void output(std::string value, bool error = false)
    {
        if (quiet_output && !error) return;
        value += '\n';
        const auto handle = GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
        DWORD mode{}, written{};
        if (GetConsoleMode(handle, &mode))
        {
            const auto text = wide(value);
            WriteConsoleW(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        }
        else WriteFile(handle, value.data(), static_cast<DWORD>(value.size()), &written, nullptr);
    }
    void string_member(Json& object, const char* key, std::string value)
    {
        object.AddMember(rapidjson::Value(key, object.GetAllocator()),
            rapidjson::Value(value.data(), static_cast<rapidjson::SizeType>(value.size()), object.GetAllocator()), object.GetAllocator());
    }
    std::string text_result(const rapidjson::Value& value, std::string prefix = {})
    {
        std::string result;
        if (value.IsObject())
        {
            for (auto item = value.MemberBegin(); item != value.MemberEnd(); ++item)
                result += text_result(item->value, prefix.empty() ? item->name.GetString() : prefix + "." + item->name.GetString());
        }
        else if (value.IsArray())
        {
            if (value.Empty()) return prefix + ": []\n";
            for (rapidjson::SizeType i = 0; i < value.Size(); ++i)
                result += text_result(value[i], prefix + "[" + std::to_string(i) + "]");
        }
        else result = prefix + ": " + (value.IsString() ? value.GetString() : serialize(value)) + "\n";
        return result;
    }
    double number(const std::wstring& value, double minimum, double maximum, bool integral)
    {
        wchar_t* end{};
        const auto parsed = wcstod(value.c_str(), &end);
        if (end == value.c_str() || *end || !std::isfinite(parsed) || parsed < minimum || parsed > maximum ||
            (integral && std::floor(parsed) != parsed)) throw Error(2, "invalid_number", "Invalid numeric argument");
        return parsed;
    }
    void verify_server(HANDLE pipe)
    {
        ULONG server_session{};
        DWORD own_session{};
        if (!GetNamedPipeServerSessionId(pipe, &server_session) ||
            !ProcessIdToSessionId(GetCurrentProcessId(), &own_session) || server_session != own_session)
            throw Error(7, "session_mismatch", "Cannot validate App login session");
    }
    HANDLE connect()
    {
        return CreateFileW(pipe_name().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    }
    HANDLE connect_or_start(bool no_start, bool quit, DWORD timeout)
    {
        auto pipe = connect();
        if (pipe != INVALID_HANDLE_VALUE) return pipe;
        const auto initial_error = GetLastError();
        if (initial_error == ERROR_ACCESS_DENIED) throw Error(7, "access_denied", "App connection denied");
        const auto deadline = GetTickCount64() + timeout;
        Handle running(OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\Glance.App"));
        if (no_start || quit)
        {
            if (initial_error == ERROR_FILE_NOT_FOUND && quit && !running.value) return INVALID_HANDLE_VALUE;
            if (initial_error != ERROR_PIPE_BUSY && !(quit && running.value)) throw Error(4, "not_running", "Glance is not running");
        }
        Handle mutex(CreateMutexW(nullptr, FALSE, (L"Local\\Glance.CLI.Start." + endpoint_suffix()).c_str()));
        HANDLE waits[]{ mutex.value, interrupted };
        const auto locked = WaitForMultipleObjects(2, waits, FALSE, timeout);
        if (locked != WAIT_OBJECT_0 && locked != WAIT_ABANDONED_0)
            throw Error(locked == WAIT_OBJECT_0 + 1 ? 130 : 5, "startup_wait", "Startup wait cancelled or timed out");
        struct Unlock { HANDLE handle; ~Unlock() { ReleaseMutex(handle); } } unlock{ mutex.value };
        pipe = connect();
        if (pipe != INVALID_HANDLE_VALUE) return pipe;
        Handle existing(OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\Glance.App"));
        if (!no_start && !quit && !existing.value)
        {
            const auto exe = (std::filesystem::path(executable_path()).parent_path() / L"Glance.exe").wstring();
            auto command = L"\"" + exe + L"\"";
            STARTUPINFOW startup{ sizeof(startup) };
            PROCESS_INFORMATION info{};
            if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                std::filesystem::path(exe).parent_path().c_str(), &startup, &info))
            {
                const auto error = GetLastError();
                if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                    throw Error(4, "app_not_found", "Glance.exe was not found beside CLI; start Glance first or place CLI beside Glance.exe");
                throw Error(4, "start_failed", "Cannot start Glance.exe beside CLI");
            }
            CloseHandle(info.hThread);
            CloseHandle(info.hProcess);
        }
        while (GetTickCount64() < deadline)
        {
            if (WaitForSingleObject(interrupted, 25) == WAIT_OBJECT_0) throw Error(130, "interrupted", "Interrupted");
            pipe = connect();
            if (pipe != INVALID_HANDLE_VALUE) return pipe;
        }
        throw Error(5, "app_timeout", "App did not become available");
    }

    Json query_window(const std::string& id, const std::string& generation = {})
    {
        Json query(rapidjson::kObjectType);
        string_member(query, "command", "window.get");
        string_member(query, "id", id);
        if (!generation.empty()) string_member(query, "generation", generation);
        Handle pipe(connect_or_start(true, false, 10000));
        verify_server(pipe.value);
        const auto deadline = GetTickCount64() + 10000;
        const auto correlation = GetTickCount64();
        if (!send(pipe.value, serialize(query), correlation, deadline, interrupted))
            throw Error(5, "send_failed", "Cannot query preview state");
        Header header;
        const auto response = receive(pipe.value, header, deadline, interrupted);
        unsigned char ack = 1;
        transfer(pipe.value, &ack, 1, true, deadline, interrupted);
        Json result;
        result.Parse(response.c_str());
        if (header.request != correlation || result.HasParseError() || !result.IsObject() ||
            !result.HasMember("ok") || !result["ok"].IsBool() || !result.HasMember("data") || !result.HasMember("error"))
            throw Error(6, "invalid_response", "Invalid preview state response");
        if (!result["ok"].GetBool())
        {
            const auto& error = result["error"];
            if (!error.IsObject() || !error.HasMember("code") || !error["code"].IsInt() ||
                !error.HasMember("name") || !error["name"].IsString() ||
                !error.HasMember("message") || !error["message"].IsString())
                throw Error(6, "invalid_response", "Invalid preview state error");
            throw Error(error["code"].GetInt(), error["name"].GetString(), error["message"].GetString());
        }
        if (!result["data"].IsObject() || !result["data"].HasMember("state") || !result["data"]["state"].IsString())
            throw Error(6, "invalid_response", "Missing preview state");
        return result;
    }
}

int wmain(int argc, wchar_t** argv)
{
    using namespace glance::cli;
    bool json = false;
    std::string command;
    Handle cancelled(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    interrupted = cancelled.value;
    SetConsoleCtrlHandler(interrupt, TRUE);
    Handle thread(OpenThread(THREAD_TERMINATE, FALSE, GetCurrentThreadId()));
    main_thread = thread.value;
    for (int i = 1; i < argc && std::wstring_view(argv[i]) != L"--"; ++i)
    {
        if (std::wstring_view(argv[i]) == L"--json") json = true;
        if (std::wstring_view(argv[i]) == L"--quiet") quiet_output = true;
        const std::wstring_view option(argv[i]);
        if (option == L"--size" || option == L"--position" || option == L"--center-offset") i += 2;
        else if (option == L"--name" || option == L"--timeout" || option == L"--id" ||
            option == L"--monitor" || option == L"--close-after") ++i;
    }
    try
    {
        Json request(rapidjson::kObjectType);
        auto& allocator = request.GetAllocator();
        bool no_start = false, wait = false, positional = false, custom_timeout = false;
        double timeout = 10;
        std::wstring input_name = L"stdin.txt";
        std::shared_ptr<InputLease> input_lease;
        std::vector<std::wstring> words;
        std::set<std::wstring> seen;
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring arg(argv[i]);
            if (!positional && arg == L"--") { positional = true; continue; }
            if (!positional && arg == L"-h") { command = "help"; continue; }
            if (positional || !arg.starts_with(L"--")) { words.push_back(arg); continue; }
            if (!seen.insert(arg).second) throw Error(2, "duplicate_option", "Duplicate option");
            const auto next = [&]() -> std::wstring {
                if (++i >= argc) throw Error(2, "missing_value", "Missing option value");
                return argv[i];
            };
            if (arg == L"--json") continue;
            if (arg == L"--quiet") continue;
            if (arg == L"--no-start") { no_start = true; continue; }
            if (arg == L"--wait") { wait = true; continue; }
            if (arg == L"--name") { input_name = next(); continue; }
            if (arg == L"--raw") continue;
            if (arg == L"--pin") { request.AddMember("pin", true, allocator); continue; }
            if (arg == L"--help") { command = "help"; continue; }
            if (arg == L"--version") { command = "version"; continue; }
            if (arg == L"--timeout") { timeout = number(next(), 0, 86400, false); custom_timeout = true; continue; }
            if (arg == L"--id")
            {
                auto value = utf8(next());
                if (value.size() != 36) throw Error(2, "invalid_id", "Window ID must be a UUID from 'preview' or 'windows'");
                for (std::size_t position = 0; position < value.size(); ++position)
                {
                    auto& character = value[position];
                    if (character >= 'A' && character <= 'F') character += 'a' - 'A';
                    const bool separator = position == 8 || position == 13 || position == 18 || position == 23;
                    if (separator ? character != '-' : !((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')))
                        throw Error(2, "invalid_id", "Window ID must be a UUID from 'preview' or 'windows'");
                }
                string_member(request, "id", value); continue;
            }
            if (arg == L"--topmost")
            {
                request.AddMember("topmost", true, allocator); continue;
            }
            if (arg == L"--monitor" || arg == L"--close-after")
            {
                const bool monitor = arg == L"--monitor";
                request.AddMember(rapidjson::StringRef(monitor ? "monitor" : "close_after"),
                    number(next(), monitor ? 0 : 0.001, monitor ? 1000 : 86400, monitor), allocator); continue;
            }
            if (arg == L"--size" || arg == L"--position" || arg == L"--center-offset")
            {
                rapidjson::Value pair(rapidjson::kArrayType);
                pair.PushBack(number(next(), arg == L"--size" ? 1 : -1000000, 1000000, true), allocator);
                pair.PushBack(number(next(), arg == L"--size" ? 1 : -1000000, 1000000, true), allocator);
                const char* key = arg == L"--size" ? "size" : arg == L"--position" ? "position" : "center_offset";
                request.AddMember(rapidjson::StringRef(key), pair, allocator); continue;
            }
            throw Error(2, "unknown_option", "Unknown option: " + utf8(arg));
        }
        const bool help_command = !words.empty() && words.front() == L"help";
        const bool command_group = words.size() == 1 &&
            (words.front() == L"window" || words.front() == L"settings") && seen.empty();
        if (command == "help" || argc == 1 || help_command || command_group)
        {
            if (help_command) words.erase(words.begin());
            std::string topic = words.empty() ? "" : utf8(words[0]);
            if ((topic == "window" || topic == "settings") && words.size() > 1)
                topic += " " + utf8(words[1]);
            const auto help = help_text(topic);
            if (json)
            {
                Json result(rapidjson::kObjectType);
                result.AddMember("schema_version", 1, result.GetAllocator());
                result.AddMember("ok", true, result.GetAllocator());
                string_member(result, "command", "help");
                Json data(rapidjson::kObjectType);
                string_member(data, "text", help);
                rapidjson::Value copy(data, result.GetAllocator());
                result.AddMember("data", copy, result.GetAllocator());
                result.AddMember("error", rapidjson::Value(), result.GetAllocator());
                output(serialize(result));
            }
            else output(help);
            return 0;
        }
        if (command == "version") { output(json ? "{\"schema_version\":1,\"ok\":true,\"command\":\"version\",\"data\":{\"version\":\"" GLANCE_VERSION_STRING "\"},\"error\":null}" : GLANCE_VERSION_STRING); return 0; }
        if (words.empty()) throw Error(2, "missing_command", "A command is required");
        command = utf8(words.front());
        words.erase(words.begin());
        if (command == "window" || command == "settings")
        {
            if (words.empty()) throw Error(2, "missing_command", "A subcommand is required");
            command += "." + utf8(words.front());
            words.erase(words.begin());
        }
        const std::set<std::string> known{ "preview", "window.get", "window.close", "window.move", "window.resize",
            "window.line", "window.page", "window.seek", "window.next", "window.previous", "window.play", "window.pause", "window.volume", "window.mute",
            "window.topmost", "window.pin", "window.set", "windows", "status", "settings.list", "settings.get", "settings.set", "settings.reset", "check-update", "quit" };
        if (!known.contains(command)) throw Error(2, "unknown_command", "Unknown command: " + command);
        std::set<std::wstring> allowed{ L"--json", L"--quiet", L"--no-start" };
        if (command == "preview" || command.starts_with("window.")) allowed.insert(L"--timeout");
        if (command.starts_with("window.")) allowed.insert(L"--id");
        if (command.starts_with("window.") && command != "window.close") allowed.insert(L"--wait");
        if (command == "preview") allowed.insert({ L"--raw", L"--name", L"--size", L"--position", L"--center-offset", L"--monitor", L"--topmost", L"--pin", L"--close-after", L"--wait" });
        if (command == "window.move") allowed.insert({ L"--position", L"--center-offset", L"--monitor" });
        if (command == "window.resize") allowed.insert(L"--size");
        if (command == "window.set") allowed.insert(L"--wait");
        for (const auto& option : seen) if (!allowed.contains(option)) throw Error(2, "invalid_option", "Option not applicable to command: " + utf8(option));
        if (request.HasMember("position") && request.HasMember("center_offset")) throw Error(2, "position_conflict", "Position modes are mutually exclusive");
        if (request.HasMember("monitor") && !request.HasMember("center_offset")) throw Error(2, "monitor_mode", "Monitor requires center-offset");
        if (command == "window.move" && !request.HasMember("position") && !request.HasMember("center_offset")) throw Error(2, "missing_position", "Position required");
        if (command == "window.resize" && !request.HasMember("size")) throw Error(2, "missing_size", "Size required");
        if (request.HasMember("pin") && request.HasMember("topmost") && !request["topmost"].GetBool()) throw Error(8, "pinned_topmost", "Pinned windows must be topmost");
        if (command == "preview" || command == "window.set")
        {
            if (words.empty()) throw Error(2, "missing_path", "At least one path is required");
            if (command == "preview" && !request.HasMember("topmost")) request.AddMember("topmost", request.HasMember("pin"), allocator);
            rapidjson::Value paths(rapidjson::kArrayType);
            const bool from_stdin = command == "preview" && std::find(words.begin(), words.end(), L"-") != words.end();
            if (seen.contains(L"--name") && !from_stdin) throw Error(2, "stdin_required", "--name requires preview -");
            if (seen.contains(L"--raw") && !from_stdin) throw Error(2, "stdin_required", "--raw requires preview -");
            if (seen.contains(L"--raw") && !seen.contains(L"--name")) input_name = L"stdin.bin";
            if (from_stdin)
            {
                if (words.size() != 1) throw Error(2, "stdin_paths", "Standard input cannot be combined with other paths");
                if (input_name.empty() || input_name == L"." || input_name == L".." ||
                    input_name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos ||
                    std::any_of(input_name.begin(), input_name.end(), [](wchar_t c) { return c < 32; }) ||
                    input_name.back() == L'.' || input_name.back() == L' ')
                    throw Error(2, "invalid_name", "--name must be a file name without directories");
                auto stem = input_name.substr(0, input_name.find(L'.'));
                std::transform(stem.begin(), stem.end(), stem.begin(), [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
                if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" || stem == L"CONIN$" || stem == L"CONOUT$" ||
                    (stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) && stem[3] >= L'1' && stem[3] <= L'9'))
                    throw Error(2, "invalid_name", "Reserved device names cannot be used for input files");
                const auto input = GetStdHandle(STD_INPUT_HANDLE);
                DWORD mode{};
                if (!input || input == INVALID_HANDLE_VALUE || GetConsoleMode(input, &mode))
                    throw Error(2, "stdin_required", "Redirect a file or pipe into preview -");
                input_lease = create_input_lease();
                const auto path = input_lease->directory / input_name;
                Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
                if (file.value == INVALID_HANDLE_VALUE) throw Error(7, "input_file", "Cannot create temporary input file");
                if (GetFileType(file.value) != FILE_TYPE_DISK) throw Error(2, "invalid_name", "Expected a regular input file");
                std::vector<char> buffer(256 * 1024);
                for (;;)
                {
                    if (WaitForSingleObject(interrupted, 0) == WAIT_OBJECT_0) throw Error(130, "interrupted", "Interrupted");
                    DWORD read{};
                    if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
                    {
                        if (GetLastError() == ERROR_BROKEN_PIPE) break;
                        throw Error(1, "stdin_read", "Cannot read standard input");
                    }
                    if (!read) break;
                    DWORD written{};
                    if (!WriteFile(file.value, buffer.data(), read, &written, nullptr) || written != read)
                        throw Error(1, "stdin_write", "Cannot write temporary input file");
                }
                words[0] = path.wstring();
                request.AddMember("stdin_file", true, allocator);
            }
            for (const auto& path : words)
            {
                const auto absolute = utf8(std::filesystem::absolute(path).lexically_normal().wstring());
                paths.PushBack(rapidjson::Value(absolute.c_str(), allocator), allocator);
            }
            request.AddMember("paths", paths, allocator);
        }
        else if (command.starts_with("settings."))
        {
            const std::size_t count = command == "settings.set" ? 2 : 1;
            if (words.size() != count && !(command == "settings.list" && words.empty())) throw Error(2, "argument_count", "Invalid settings arguments");
            if (!words.empty()) string_member(request, "key", utf8(words[0]));
            if (words.size() == 2) string_member(request, "value", utf8(words[1]));
        }
        else if (command == "window.line" || command == "window.page" || command == "window.seek" || command == "window.volume")
        {
            if (words.size() != 1) throw Error(2, "argument_count", "Expected one numeric value");
            double value{};
            if (command == "window.seek" && words[0].find(L':') != std::wstring::npos)
            {
                const auto first = words[0].find(L':');
                const auto second = words[0].find(L':', first + 1);
                if (second == std::wstring::npos || words[0].find(L':', second + 1) != std::wstring::npos)
                    throw Error(2, "invalid_position", "Use seconds or HH:MM:SS[.fff]");
                value = number(words[0].substr(0, first), 0, 100000, true) * 3600 +
                    number(words[0].substr(first + 1, second - first - 1), 0, 59, true) * 60 +
                    number(words[0].substr(second + 1), 0, 59.999999, false);
            }
            else value = number(words[0], command == "window.line" || command == "window.page" ? 1 : 0,
                command == "window.volume" ? 100 : INT32_MAX, command != "window.seek" && command != "window.volume");
            request.AddMember("value", value, allocator);
        }
        else if (command == "window.pin" || command == "window.topmost" || command == "window.mute")
        {
            if (words.size() != 1 || (words[0] != L"on" && words[0] != L"off")) throw Error(2, "invalid_boolean", "Expected on or off");
            request.AddMember("enabled", words[0] == L"on", allocator);
        }
        else if (!words.empty()) throw Error(2, "argument_count", "Unexpected positional arguments");
        const DWORD transport_timeout = command == "check-update" ? 60000 : 10000;
        request.AddMember("timeout_ms", static_cast<unsigned>(transport_timeout), allocator);
        string_member(request, "command", command);
        const auto payload = serialize(request);
        if (payload.size() > maximum_payload) throw Error(2, "request_too_large", "Request exceeds size limit");
        Handle pipe(connect_or_start(no_start, command == "quit", 15000));
        if (pipe.value == INVALID_HANDLE_VALUE)
        {
            output(json ? "{\"schema_version\":1,\"ok\":true,\"command\":\"quit\",\"data\":{\"running\":false},\"error\":null}" : "OK"); return 0;
        }
        verify_server(pipe.value);
        const auto wait_started = GetTickCount64();
        const auto deadline = wait_started + transport_timeout;
        const auto correlation = GetTickCount64() ^ (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32);
        if (!send(pipe.value, payload, correlation, deadline, interrupted)) throw Error(5, "send_failed", "Request was not delivered");
        Header header;
        auto response = receive(pipe.value, header, deadline, interrupted);
        unsigned char ack = 1;
        transfer(pipe.value, &ack, 1, true, deadline, interrupted);
        if (header.request != correlation) throw Error(6, "request_mismatch", "Unexpected response ID");
        Json result;
        result.Parse(response.c_str());
        if (result.HasParseError() || !result.IsObject() || !result.HasMember("ok") || !result["ok"].IsBool() ||
            !result.HasMember("schema_version") || !result["schema_version"].IsInt() || result["schema_version"].GetInt() != 1 ||
            !result.HasMember("data") || !result.HasMember("error"))
            throw Error(6, "invalid_response", "Invalid App response");
        const bool ok = result["ok"].GetBool();
        if (ok ? !result["data"].IsObject() : (!result["error"].IsObject() ||
            !result["error"].HasMember("code") || !result["error"]["code"].IsInt() ||
            !result["error"].HasMember("name") || !result["error"]["name"].IsString() ||
            !result["error"].HasMember("message") || !result["error"]["message"].IsString()))
            throw Error(6, "invalid_response", "Invalid App result fields");
        if (ok && (command == "preview" || command == "window.pin") &&
            (!result["data"].HasMember("id") || !result["data"]["id"].IsString()))
            throw Error(6, "invalid_response", "App response lacks a window ID");
        int code = ok ? 0 : result["error"]["code"].GetInt();
        if (ok && result["data"].HasMember("id") && result["data"].HasMember("state"))
        {
            const std::string id = result["data"]["id"].GetString();
            const std::string generation = result["data"]["generation"].GetString();
            bool completed = false;
            for (;;)
            {
                if (result["data"].HasMember("command_error_code"))
                    throw Error(result["data"]["command_error_code"].GetInt(), "content_control_failed", result["data"]["command_error_message"].GetString());
                const std::string_view state = result["data"]["state"].GetString();
                if (state == "failed") throw Error(9, "preview_failed", "Preview provider failed");
                const bool closed = state == "closed";
                completed = wait ? closed : state != "loading";
                if (completed) break;
                if (custom_timeout && GetTickCount64() - wait_started >= static_cast<ULONGLONG>(timeout * 1000)) break;
                if (WaitForSingleObject(interrupted, 50) == WAIT_OBJECT_0) throw Error(130, "interrupted", "Interrupted");
                try
                {
                    auto latest = query_window(id, wait || command == "window.next" || command == "window.previous" ? "" : generation);
                    result["data"].CopyFrom(latest["data"], result.GetAllocator());
                }
                catch (const Error& error)
                {
                    if (error.code != 3) throw;
                    if (!wait) throw Error(10, "preview_closed", "Preview closed before completion");
                    result["data"]["state"].SetString("closed", result.GetAllocator());
                    completed = true;
                    break;
                }
            }
            if (wait && completed)
            {
                result["data"]["state"].SetString("closed", result.GetAllocator());
            }
            result["data"].AddMember("wait_completed", completed, result.GetAllocator());
            response = serialize(result);
        }
        if (command == "quit" && ok && result["data"].HasMember("process_id"))
        {
            Handle process(OpenProcess(SYNCHRONIZE, FALSE, result["data"]["process_id"].GetUint()));
            if (process.value && WaitForSingleObject(process.value, transport_timeout) == WAIT_TIMEOUT)
                throw Error(5, "shutdown_timeout", "App did not exit");
            if (result["data"].HasMember("core_process_id"))
            {
                Handle core(OpenProcess(SYNCHRONIZE, FALSE, result["data"]["core_process_id"].GetUint()));
                if (core.value && WaitForSingleObject(core.value, transport_timeout) == WAIT_TIMEOUT)
                    throw Error(5, "shutdown_timeout", "Core did not exit");
            }
        }
        if (json) output(response, quiet_output && !ok);
        else if (!ok) output(std::string(result["error"]["name"].GetString()) + ": " + result["error"]["message"].GetString(), true);
        else if (result["data"].HasMember("text")) output(result["data"]["text"].GetString());
        else
        {
            auto text = text_result(result["data"]);
            if (!text.empty()) text.pop_back();
            output(text.empty() ? "OK" : text);
        }
        return code;
    }
    catch (const std::exception& exception)
    {
        const auto typed = dynamic_cast<const Error*>(&exception);
        const bool cancelled_now = WaitForSingleObject(interrupted, 0) == WAIT_OBJECT_0;
        const int code = cancelled_now ? 130 : typed ? typed->code : 1;
        const std::string name = cancelled_now ? "interrupted" : typed ? typed->name : "internal_error";
        if (json)
        {
            Json result(rapidjson::kObjectType);
            result.AddMember("schema_version", 1, result.GetAllocator());
            result.AddMember("ok", false, result.GetAllocator());
            string_member(result, "command", command);
            result.AddMember("data", rapidjson::Value(), result.GetAllocator());
            Json error(rapidjson::kObjectType);
            error.AddMember("code", code, error.GetAllocator());
            string_member(error, "name", name);
            string_member(error, "message", exception.what());
            rapidjson::Value copy(error, result.GetAllocator());
            result.AddMember("error", copy, result.GetAllocator());
            output(serialize(result), quiet_output);
        }
        else output(name + ": " + exception.what(), true);
        return code;
    }
}

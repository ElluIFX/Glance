#include "glance/contracts/cli_protocol.h"
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
    BOOL WINAPI interrupt(DWORD event)
    {
        if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
        SetEvent(interrupted);
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
        ULONG pid{};
        if (!GetNamedPipeServerProcessId(pipe, &pid)) throw Error(4, "server_unavailable", "Cannot identify App");
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        std::wstring actual(32768, L'\0');
        DWORD count = static_cast<DWORD>(actual.size());
        if (!QueryFullProcessImageNameW(process.value, 0, actual.data(), &count))
            throw Error(7, "server_identity", "Cannot validate App identity");
        actual.resize(count);
        const auto expected = (std::filesystem::path(executable_path()).parent_path() / L"Glance.exe").wstring();
        if (_wcsicmp(expected.c_str(), actual.c_str()) != 0)
            throw Error(8, "installation_mismatch", "An App from another installation is running");
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
                throw Error(4, "start_failed", "Cannot start Glance.exe beside CLI");
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
}

int wmain(int argc, wchar_t** argv)
{
    using namespace glance::cli;
    bool json = false;
    std::string command;
    Handle cancelled(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    interrupted = cancelled.value;
    SetConsoleCtrlHandler(interrupt, TRUE);
    for (int i = 1; i < argc && std::wstring_view(argv[i]) != L"--"; ++i)
        if (std::wstring_view(argv[i]) == L"--json") json = true;
    try
    {
        Json request(rapidjson::kObjectType);
        auto& allocator = request.GetAllocator();
        bool no_start = false, wait = false, positional = false, custom_timeout = false;
        double timeout = 10;
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
            if (arg == L"--no-start") { no_start = true; continue; }
            if (arg == L"--wait") { wait = true; request.AddMember("wait", true, allocator); continue; }
            if (arg == L"--pin") { request.AddMember("pin", true, allocator); continue; }
            if (arg == L"--help") { command = "help"; continue; }
            if (arg == L"--version") { command = "version"; continue; }
            if (arg == L"--timeout") { timeout = number(next(), 0.001, 86400, false); custom_timeout = true; continue; }
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
            "window.topmost", "window.pin", "window.set", "windows", "status", "settings.list", "settings.get", "settings.set", "settings.reset", "check-update", "quit" };
        if (!known.contains(command)) throw Error(2, "unknown_command", "Unknown command: " + command);
        std::set<std::wstring> allowed{ L"--json", L"--no-start", L"--timeout" };
        if (command.starts_with("window.")) allowed.insert(L"--id");
        if (command == "preview") allowed.insert({ L"--size", L"--position", L"--center-offset", L"--monitor", L"--topmost", L"--pin", L"--close-after", L"--wait" });
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
        else if (command == "window.pin" || command == "window.topmost")
        {
            if (words.size() != 1 || (words[0] != L"on" && words[0] != L"off")) throw Error(2, "invalid_boolean", "Expected on or off");
            request.AddMember("enabled", words[0] == L"on", allocator);
        }
        else if (!words.empty()) throw Error(2, "argument_count", "Unexpected positional arguments");
        if (!custom_timeout) timeout = command == "check-update" ? 60 : wait ? 30 : 10;
        request.AddMember("timeout_ms", static_cast<std::uint32_t>(timeout * 1000), allocator);
        string_member(request, "command", command);
        const auto payload = serialize(request);
        if (payload.size() > maximum_payload) throw Error(2, "request_too_large", "Request exceeds size limit");
        Handle pipe(connect_or_start(no_start, command == "quit", static_cast<DWORD>(custom_timeout ? timeout * 1000 : 15000)));
        if (pipe.value == INVALID_HANDLE_VALUE)
        {
            output(json ? "{\"schema_version\":1,\"ok\":true,\"command\":\"quit\",\"data\":{\"running\":false},\"error\":null}" : "OK"); return 0;
        }
        verify_server(pipe.value);
        const auto deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout * 1000);
        const auto correlation = GetTickCount64() ^ (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32);
        if (!send(pipe.value, payload, correlation, deadline, interrupted)) throw Error(5, "send_failed", "Request was not delivered");
        Header header;
        const auto response = receive(pipe.value, header, deadline, interrupted);
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
        if (command == "quit" && ok && result["data"].HasMember("process_id"))
        {
            Handle process(OpenProcess(SYNCHRONIZE, FALSE, result["data"]["process_id"].GetUint()));
            if (process.value && WaitForSingleObject(process.value, static_cast<DWORD>(timeout * 1000)) == WAIT_TIMEOUT)
                throw Error(5, "shutdown_timeout", "App did not exit");
            if (result["data"].HasMember("core_process_id"))
            {
                Handle core(OpenProcess(SYNCHRONIZE, FALSE, result["data"]["core_process_id"].GetUint()));
                if (core.value && WaitForSingleObject(core.value, static_cast<DWORD>(timeout * 1000)) == WAIT_TIMEOUT)
                    throw Error(5, "shutdown_timeout", "Core did not exit");
            }
        }
        if (json) output(response);
        else if (!ok) output(std::string(result["error"]["name"].GetString()) + ": " + result["error"]["message"].GetString(), true);
        else if (command == "preview" || command == "window.pin") output(result["data"]["id"].GetString());
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
            output(serialize(result));
        }
        else output(name + ": " + exception.what(), true);
        return code;
    }
}

#include "pch.h"
#include "glance/contracts/component_api.h"
#include "../Common/preview_cancellation.h"
#include "../../version.h"

#include <filesystem>
#include <string>

namespace
{
    using namespace glance::contracts::components;
    using namespace winrt::Windows::Data::Json;

    std::filesystem::path directory()
    {
        HMODULE module{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&directory), &module)) return {};
        std::wstring path(32768, L'\0');
        const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size()) return {};
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

    BOOL WINAPI initialize(const ComponentRegistrar*, ComponentRegistration* registration) noexcept
    {
        if (!registration || registration->size < sizeof(*registration)) return FALSE;
        ComponentRegistration result;
        wcscpy_s(result.component_id, L"file-format");
        wcscpy_s(result.target_app_version, GLANCE_VERSION_WSTRING);
        wcscpy_s(result.resource_path, L"resources.pri");
        *registration = result;
        return TRUE;
    }

    BOOL WINAPI query_status(ComponentStatusResult* result) noexcept
    {
        if (!result || result->size < sizeof(*result)) return FALSE;
        try
        {
            ComponentStatusResult status;
            wcscpy_s(status.display_name_key, L"ComponentName");
            const auto root = directory();
            const bool available = std::filesystem::is_regular_file(root / L"Glance.FileFormatHost.exe") &&
                std::filesystem::is_regular_file(root / L"default.sig") && std::filesystem::is_regular_file(root / L"formats.json");
            status.severity = available ? HealthSeverity::healthy : HealthSeverity::error;
            wcscpy_s(status.detail_key, available ? L"ComponentDescription" : L"ComponentUnavailable");
            *result = status;
            return TRUE;
        }
        catch (...) { return FALSE; }
    }

    template<std::size_t N>
    void copy(wchar_t (&destination)[N], std::wstring_view source)
    {
        const auto count = (std::min)(source.size(), N - 1);
        std::copy_n(source.data(), count, destination);
        destination[count] = L'\0';
    }

    void message(const InformationPanelSink* sink, const wchar_t* key)
    {
        InformationPanelEntry entry;
        entry.kind = InformationPanelEntryKind::section;
        entry.label.kind = ComponentTextKind::resource_key;
        entry.value.kind = ComponentTextKind::literal;
        copy(entry.label.value, key);
        sink->append(sink->context, &entry);
    }

    PrepareStatus WINAPI identify(const wchar_t* path, const PreviewCancellation* cancellation,
        const InformationPanelSink* information, const FileFormatSink* sink) noexcept
    {
        if (!path || !information || !information->append || !sink || !sink->append) return PrepareStatus::failed;
        try
        {
            glance::components::PreparationScope scope(cancellation);
            if (glance::components::preview_cancelled()) return PrepareStatus::cancelled;
            winrt::handle job(CreateJobObjectW(nullptr, nullptr));
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
            limits.ProcessMemoryLimit = 512ULL * 1024 * 1024;
            if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return PrepareStatus::failed;
            SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
            HANDLE read{}, write{};
            if (!CreatePipe(&read, &write, &security, 0)) return PrepareStatus::failed;
            winrt::handle input(read), output(write);
            if (!SetHandleInformation(input.get(), HANDLE_FLAG_INHERIT, 0)) return PrepareStatus::failed;
            const auto executable = directory() / L"Glance.FileFormatHost.exe";
            std::wstring command = L"\"" + executable.wstring() + L"\" \"" + path + L"\"";
            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdOutput = output.get();
            startup.StartupInfo.hStdError = output.get();
            SIZE_T bytes{};
            InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
            std::vector<std::byte> attributes(bytes);
            startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
            if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes)) return PrepareStatus::failed;
            struct AttributeCleanup
            {
                LPPROC_THREAD_ATTRIBUTE_LIST value;
                ~AttributeCleanup() { DeleteProcThreadAttributeList(value); }
            } cleanup{startup.lpAttributeList};
            HANDLE handles[]{output.get()};
            if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                handles, sizeof(handles), nullptr, nullptr)) return PrepareStatus::failed;
            PROCESS_INFORMATION process{};
            if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | BELOW_NORMAL_PRIORITY_CLASS,
                nullptr, nullptr, &startup.StartupInfo, &process)) return PrepareStatus::failed;
            winrt::handle child(process.hProcess), thread(process.hThread);
            if (!AssignProcessToJobObject(job.get(), child.get()))
            {
                TerminateProcess(child.get(), ERROR_CANCELLED);
                return PrepareStatus::failed;
            }
            if (ResumeThread(thread.get()) == DWORD(-1)) return PrepareStatus::failed;
            output.close();
            auto start = GetTickCount64();
            bool ready{};
            std::string text;
            bool finished{};
            for (;;)
            {
                if (glance::components::preview_cancelled()) return PrepareStatus::cancelled;
                if (GetTickCount64() - start >= (ready ? 15000ULL : 30000ULL))
                {
                    message(information, L"IdentificationLimited");
                    return PrepareStatus::success;
                }
                DWORD available{};
                if (!PeekNamedPipe(input.get(), nullptr, 0, nullptr, &available, nullptr))
                {
                    if (GetLastError() != ERROR_BROKEN_PIPE) return PrepareStatus::failed;
                    if (WaitForSingleObject(child.get(), 10) == WAIT_OBJECT_0) break;
                    continue;
                }
                if (available)
                {
                    char buffer[4096];
                    DWORD count{};
                    if (!ReadFile(input.get(), buffer, (std::min)(available, DWORD(sizeof(buffer))), &count, nullptr)) break;
                    text.append(buffer, count);
                    if (!ready && text.starts_with("ready\n"))
                    {
                        text.erase(0, 6);
                        ready = true;
                        start = GetTickCount64();
                    }
                    if (text.size() > 128 * 1024) return PrepareStatus::failed;
                    continue;
                }
                if (finished) break;
                finished = WaitForSingleObject(child.get(), 10) == WAIT_OBJECT_0;
            }
            DWORD exit_code{};
            if (!GetExitCodeProcess(child.get(), &exit_code) || exit_code != 0) return PrepareStatus::failed;
            const auto json = JsonObject::Parse(winrt::to_hstring(text));
            const auto status = json.GetNamedString(L"status");
            if (status != L"success")
            {
                message(information, status == L"limited" ? L"IdentificationLimited" : L"IdentificationFailed");
                return PrepareStatus::success;
            }
            const auto candidates = json.GetNamedValue(L"candidates");
            if (candidates.ValueType() == JsonValueType::Null || candidates.GetArray().Size() == 0)
                message(information, L"IdentificationUnknown");
            else for (const auto& value : candidates.GetArray())
            {
                const auto item = value.GetObject();
                FileFormatCandidate candidate;
                copy(candidate.name, item.GetNamedString(L"name"));
                copy(candidate.version, item.GetNamedString(L"version"));
                copy(candidate.mime, item.GetNamedString(L"mime"));
                copy(candidate.identifier, item.GetNamedString(L"id"));
                copy(candidate.basis, item.GetNamedString(L"basis"));
                std::wstring extensions;
                const auto values = item.GetNamedValue(L"extensions");
                if (values.ValueType() == JsonValueType::Array) for (const auto& extension : values.GetArray())
                {
                    if (!extensions.empty()) extensions += L";";
                    extensions += extension.GetString();
                }
                copy(candidate.extensions, extensions);
                if (!sink->append(sink->context, &candidate)) return PrepareStatus::failed;
            }
            return PrepareStatus::success;
        }
        catch (...) { return PrepareStatus::failed; }
    }

    const InformationProviderApi information_api{.identify_format = identify};
    BOOL WINAPI query_interface(const GUID* id, std::uint32_t version, void** output) noexcept
    {
        if (!output) return FALSE;
        *output = nullptr;
        if (id && IsEqualGUID(*id, information_provider_api_id) && version <= information_provider_api_version)
        {
            *output = const_cast<InformationProviderApi*>(&information_api);
            return TRUE;
        }
        return FALSE;
    }
    void WINAPI shutdown() noexcept {}
}

extern "C" __declspec(dllexport) BOOL WINAPI GlanceComponentGetApi(
    std::uint32_t abi, glance::contracts::components::ComponentApi* api) noexcept
{
    using namespace glance::contracts::components;
    if (abi != abi_version || !api || api->size < sizeof(*api)) return FALSE;
    ComponentApi result;
    result.initialize = initialize;
    result.query_status = query_status;
    result.query_interface = query_interface;
    result.shutdown = shutdown;
    *api = result;
    return TRUE;
}

#include "pch.h"

#include "glance/contracts/component_api.h"
#include "office_availability.h"
#include "../Common/component_localization.h"
#include "../../version.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cwchar>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
    using namespace glance::contracts::components;

    constexpr std::array office_extensions{
        L".doc", L".docx", L".xls", L".xlsx", L".ppt", L".pptx" };
    constexpr wchar_t display_name_key[] = L"Component.DisplayName";
    constexpr wchar_t status_available_key[] = L"Status.Available";
    constexpr wchar_t status_unavailable_key[] = L"Status.Unavailable";
    constexpr wchar_t status_partial_key[] = L"Status.Partial";
    constexpr wchar_t loading_key[] = L"Preview.Loading";
    constexpr wchar_t protected_source_notice_key[] =
        L"Preview.ProtectedSourceNotice";

    glance::components::ComponentResourceStore component_resources;
    std::mutex preview_lease_mutex;
    std::unordered_map<std::uint64_t, std::filesystem::path> preview_leases;
    std::atomic_uint64_t next_preview_lease{ 1 };

    template <std::size_t Size>
    bool localize(
        const wchar_t* key,
        const wchar_t* language_tag,
        wchar_t (&destination)[Size]) noexcept
    {
        return component_resources.copy(key, language_tag, destination, Size);
    }

    std::filesystem::path component_directory() noexcept
    {
        HMODULE module{};
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&component_directory),
                &module))
        {
            return {};
        }
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(
            module,
            path.data(),
            static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size())
        {
            return {};
        }
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

    BOOL WINAPI query_host(NativePreviewHostDescriptor* descriptor) noexcept
    {
        if (descriptor == nullptr ||
            descriptor->size < sizeof(NativePreviewHostDescriptor))
        {
            return FALSE;
        }
        std::error_code error;
        if (!std::filesystem::is_regular_file(
                component_directory() / L"Glance.OfficeHost.exe",
                error))
        {
            return FALSE;
        }
        NativePreviewHostDescriptor result;
        wcscpy_s(result.host_executable, L"Glance.OfficeHost.exe");
        *descriptor = result;
        return TRUE;
    }

    const NativePreviewRendererApi native_preview_api{
        .query_host = query_host };

    bool is_protected_source(const std::filesystem::path& source) noexcept
    {
        try
        {
            const auto zone_path = source.wstring() + L":Zone.Identifier";
            const auto zone = GetPrivateProfileIntW(
                L"ZoneTransfer",
                L"ZoneId",
                0,
                zone_path.c_str());
            return zone >= 3;
        }
        catch (...)
        {
            return false;
        }
    }

    std::pair<std::filesystem::path, std::uint64_t> create_preview_copy(
        const std::filesystem::path& source)
    {
        std::error_code error;
        const auto token = next_preview_lease.fetch_add(1, std::memory_order_relaxed);
        const auto directory = std::filesystem::temp_directory_path() /
            L"Glance" / L"OfficePreview" /
            (std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(token));
        std::filesystem::create_directories(directory, error);
        if (error)
        {
            return {};
        }
        const auto destination = directory / (L"preview" + source.extension().wstring());
        if (!std::filesystem::copy_file(
                source,
                destination,
                std::filesystem::copy_options::overwrite_existing,
                error) ||
            error)
        {
            std::filesystem::remove_all(directory, error);
            return {};
        }
        const auto copied_zone = destination.wstring() + L":Zone.Identifier";
        static_cast<void>(DeleteFileW(copied_zone.c_str()));
        {
            std::scoped_lock lock(preview_lease_mutex);
            preview_leases.emplace(token, directory);
        }
        return { destination, token };
    }

    BOOL WINAPI initialize(
        const ComponentRegistrar* registrar,
        ComponentRegistration* registration) noexcept
    {
        if (registrar == nullptr ||
            registrar->size < sizeof(ComponentRegistrar) ||
            registrar->register_extension == nullptr ||
            registrar->register_renderer == nullptr ||
            registration == nullptr ||
            registration->size < sizeof(ComponentRegistration) ||
            !component_resources.initialize())
        {
            return FALSE;
        }

        glance::app::initialize_office_availability();
        for (const auto* extension : office_extensions)
        {
            if (!registrar->register_extension(registrar->context, extension))
            {
                return FALSE;
            }
        }
        if (!registrar->register_renderer(
                registrar->context,
                PreviewContentKind::document,
                PreviewContentFormat::native_surface,
                &native_preview_renderer_api_id,
                native_preview_renderer_api_version))
        {
            return FALSE;
        }

        ComponentRegistration result;
        wcscpy_s(result.component_id, L"office");
        wcscpy_s(result.target_app_version, GLANCE_VERSION_WSTRING);
        result.preferred_kind = PreviewContentKind::document;
        result.preferred_format = PreviewContentFormat::native_surface;
        *registration = result;
        return TRUE;
    }

    BOOL WINAPI query_status(
        const wchar_t* language_tag,
        ComponentStatusResult* result) noexcept
    {
        if (result == nullptr || result->size < sizeof(ComponentStatusResult))
        {
            return FALSE;
        }

        const auto mask = glance::app::office_available_components();
        ComponentStatusResult status;
        status.severity = mask == glance::app::office_all_components
            ? HealthSeverity::healthy
            : HealthSeverity::warning;
        status.capability_mask = mask;
        if (!localize(display_name_key, language_tag, status.display_name))
        {
            return FALSE;
        }
        const wchar_t* detail_key = mask == glance::app::office_all_components
            ? status_available_key
            : mask == 0 ? status_unavailable_key : status_partial_key;
        if (!localize(detail_key, language_tag, status.detail))
        {
            return FALSE;
        }
        *result = status;
        return TRUE;
    }

    BOOL WINAPI query_loading_text(
        const wchar_t* path,
        const wchar_t* language_tag,
        ComponentLoadingTextResult* result) noexcept
    {
        if (path == nullptr || result == nullptr ||
            result->size < sizeof(ComponentLoadingTextResult))
        {
            return FALSE;
        }
        ComponentLoadingTextResult loading;
        if (!localize(loading_key, language_tag, loading.text))
        {
            return FALSE;
        }
        *result = loading;
        return TRUE;
    }

    BOOL WINAPI can_preview(const wchar_t* path) noexcept
    {
        return path != nullptr && glance::app::office_preview_available(path);
    }

    PrepareStatus prepare_preview_impl(
        const wchar_t* path,
        const wchar_t*,
        PreparedPreview* preview) noexcept
    {
        if (path == nullptr || preview == nullptr ||
            preview->size < sizeof(PreparedPreview))
        {
            return PrepareStatus::failed;
        }
        try
        {
            const std::filesystem::path source(path);
            std::error_code error;
            const auto value = source.wstring();
            if (!source.is_absolute() ||
                !std::filesystem::is_regular_file(source, error) ||
                !glance::app::office_preview_available(value))
            {
                return PrepareStatus::unavailable;
            }
            const bool protected_source = is_protected_source(source);
            auto output = source;
            std::uint64_t lease_token{};
            if (protected_source)
            {
                auto prepared_copy = create_preview_copy(source);
                output = std::move(prepared_copy.first);
                lease_token = prepared_copy.second;
                if (output.empty())
                {
                    return PrepareStatus::failed;
                }
            }
            const auto output_value = output.wstring();
            if (output_value.size() + 1 > preview_path_capacity)
            {
                if (lease_token != 0)
                {
                    std::filesystem::path directory;
                    {
                        std::scoped_lock lock(preview_lease_mutex);
                        const auto lease = preview_leases.find(lease_token);
                        if (lease != preview_leases.end())
                        {
                            directory = std::move(lease->second);
                            preview_leases.erase(lease);
                        }
                    }
                    std::filesystem::remove_all(directory, error);
                }
                return PrepareStatus::failed;
            }
            PreparedPreview prepared;
            prepared.kind = PreviewContentKind::document;
            prepared.format = PreviewContentFormat::native_surface;
            prepared.lease_token = lease_token;
            std::copy(output_value.begin(), output_value.end(), prepared.path);
            prepared.path[output_value.size()] = L'\0';
            *preview = prepared;
            return PrepareStatus::success;
        }
        catch (...)
        {
            return PrepareStatus::failed;
        }
    }

    PrepareStatus WINAPI prepare_preview(
        const wchar_t* path,
        const wchar_t* language_tag,
        PreparedPreview* preview) noexcept
    {
        return prepare_preview_impl(path, language_tag, preview);
    }

    BOOL WINAPI query_preview_notice(
        std::uint64_t lease_token,
        const wchar_t* language_tag,
        PreviewNoticeResult* result) noexcept
    {
        if (lease_token == 0 || result == nullptr ||
            result->size < sizeof(PreviewNoticeResult))
        {
            return FALSE;
        }
        {
            std::scoped_lock lock(preview_lease_mutex);
            if (!preview_leases.contains(lease_token))
            {
                return FALSE;
            }
        }
        PreviewNoticeResult notice;
        notice.severity = PreviewNoticeSeverity::warning;
        notice.duration_ms = 1000;
        if (!localize(
                protected_source_notice_key,
                language_tag,
                notice.text))
        {
            return FALSE;
        }
        *result = notice;
        return TRUE;
    }

    void WINAPI release_preview(std::uint64_t token) noexcept
    {
        std::filesystem::path directory;
        {
            std::scoped_lock lock(preview_lease_mutex);
            const auto lease = preview_leases.find(token);
            if (lease != preview_leases.end())
            {
                directory = std::move(lease->second);
                preview_leases.erase(lease);
            }
        }
        if (!directory.empty())
        {
            std::error_code error;
            std::filesystem::remove_all(directory, error);
        }
    }

    const PreviewNoticeApi preview_notice_api{
        .query_preview_notice = query_preview_notice };

    BOOL WINAPI query_interface(
        const GUID* interface_id,
        std::uint32_t minimum_version,
        void** interface_pointer) noexcept
    {
        if (interface_id == nullptr || interface_pointer == nullptr)
        {
            return FALSE;
        }
        *interface_pointer = nullptr;
        if (IsEqualGUID(*interface_id, native_preview_renderer_api_id) &&
            minimum_version <= native_preview_renderer_api_version)
        {
            *interface_pointer = const_cast<NativePreviewRendererApi*>(
                &native_preview_api);
            return TRUE;
        }
        if (IsEqualGUID(*interface_id, preview_notice_api_id) &&
            minimum_version <= preview_notice_api_version)
        {
            *interface_pointer = const_cast<PreviewNoticeApi*>(
                &preview_notice_api);
            return TRUE;
        }
        return FALSE;
    }

    void WINAPI shutdown() noexcept
    {
        std::vector<std::filesystem::path> directories;
        {
            std::scoped_lock lock(preview_lease_mutex);
            directories.reserve(preview_leases.size());
            for (auto& entry : preview_leases)
            {
                directories.push_back(std::move(entry.second));
            }
            preview_leases.clear();
        }
        for (const auto& directory : directories)
        {
            std::error_code error;
            std::filesystem::remove_all(directory, error);
        }
        component_resources.shutdown();
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI GlanceComponentGetApi(
    std::uint32_t host_abi,
    glance::contracts::components::ComponentApi* api) noexcept
{
    using namespace glance::contracts::components;
    if (host_abi != abi_version || api == nullptr ||
        api->size < sizeof(ComponentApi))
    {
        return FALSE;
    }
    ComponentApi result;
    result.initialize = initialize;
    result.query_status = query_status;
    result.query_loading_text = query_loading_text;
    result.can_preview = can_preview;
    result.prepare_preview = prepare_preview;
    result.release_preview = release_preview;
    result.query_interface = query_interface;
    result.shutdown = shutdown;
    *api = result;
    return TRUE;
}

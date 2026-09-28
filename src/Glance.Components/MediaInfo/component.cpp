#include "pch.h"

#include "glance/contracts/component_api.h"
#include "glance/contracts/dependency_api.h"
#include "media_probe.h"
#include "../Common/component_text.h"
#include "../../version.h"

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <mutex>

namespace
{
    using namespace glance::contracts::components;

    constexpr wchar_t component_id[] = L"media-info";
    constexpr wchar_t shortcut_id[] = L"advanced-media-info";
    constexpr wchar_t hover_info_id[] = L"advanced-media-info";
    constexpr wchar_t archive_url[] =
        L"https://www.gyan.dev/ffmpeg/builds/packages/ffmpeg-8.1.2-essentials_build.7z";
    constexpr wchar_t archive_file_name[] = L"ffmpeg-8.1.2-essentials_build.7z";
    constexpr wchar_t archive_sha256[] =
        L"e25b682664025d49034c981afb4bae36238a40f29a3cc1c713ad9a8b5b3528f6";
    constexpr std::uint64_t archive_size = 33876939;

    const glance::contracts::dependencies::HostApi* dependencies{};

    bool available() noexcept
    {
        if (!dependencies) return false;
        const auto state = dependencies->query(L"ffprobe", L"ffprobe");
        return state == glance::contracts::dependencies::Availability::managed ||
            state == glance::contracts::dependencies::Availability::external;
    }

    BOOL WINAPI initialize(
        const ComponentRegistrar* registrar,
        ComponentRegistration* registration) noexcept
    {
        if (registrar == nullptr || registrar->size < sizeof(ComponentRegistrar) ||
            registration == nullptr || registration->size < sizeof(ComponentRegistration))
        {
            return FALSE;
        }

        void* service{};
        if (!registrar->query_host_interface || !registrar->query_host_interface(
            &glance::contracts::dependencies::host_api_id, glance::contracts::dependencies::host_api_version, &service))
            return FALSE;
        dependencies = static_cast<const glance::contracts::dependencies::HostApi*>(service);
        const glance::contracts::dependencies::File files[]{
            {L"ffmpeg-8.1.2-essentials_build/bin/ffprobe.exe", L"bin/ffprobe.exe",
             L"b49ccc7c6547b141ad5a2f6ec69cc04323d7133d7704d70b331b904c63eecb07"},
            {L"ffmpeg-8.1.2-essentials_build/LICENSE", L"LICENSE",
             L"8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903"}};
        const glance::contracts::dependencies::Entry entries[]{
            {L"ffprobe", glance::contracts::dependencies::EntryKind::executable, L"bin/ffprobe.exe", L"ffprobe.exe",
             L"Components/media-info/bin/ffprobe.exe"}};
        const glance::contracts::dependencies::Declaration dependency{
            .id = L"ffprobe", .version = L"8.1.2", .display_name = L"FFprobe",
            .url = archive_url, .archive_name = archive_file_name, .sha256 = archive_sha256,
            .archive_size = archive_size, .files = files, .file_count = 2, .entries = entries, .entry_count = 1,
            .description_key = L"Dependency.Description"};
        if (FAILED(dependencies->register_dependency(&dependency, component_id))) return FALSE;

        ComponentRegistration result;
        wcscpy_s(result.component_id, component_id);
        wcscpy_s(result.target_app_version, GLANCE_VERSION_WSTRING);
        wcscpy_s(result.resource_path, L"resources.pri");
        result.preferred_kind = PreviewContentKind::none;
        result.preferred_format = PreviewContentFormat::none;
        *registration = result;
        return TRUE;
    }

    BOOL WINAPI query_status(ComponentStatusResult* result) noexcept
    {
        if (result == nullptr || result->size < sizeof(ComponentStatusResult))
        {
            return FALSE;
        }
        ComponentStatusResult status;
        const bool is_available = available();
        status.severity = is_available ? HealthSeverity::healthy : HealthSeverity::error;
        status.capability_mask = is_available ? 1 : 0;
        if (!glance::components::copy_resource_key(
                L"Component.DisplayName",
                status.display_name_key) ||
            !glance::components::copy_resource_key(
                is_available ? L"Status.Available" : L"Status.Unavailable",
                status.detail_key))
        {
            return FALSE;
        }
        *result = status;
        return TRUE;
    }

    BOOL WINAPI enumerate_shortcuts(
        StatusBarShortcutDescriptor* descriptors,
        std::uint32_t capacity,
        std::uint32_t* count) noexcept
    {
        if (count == nullptr)
        {
            return FALSE;
        }
        *count = 1;
        if (descriptors == nullptr || capacity == 0)
        {
            return TRUE;
        }
        if (capacity < 1 || descriptors[0].size < sizeof(StatusBarShortcutDescriptor))
        {
            return FALSE;
        }
        StatusBarShortcutDescriptor descriptor;
        wcscpy_s(descriptor.shortcut_id, shortcut_id);
        descriptor.target_kind = PreviewContentKind::media;
        descriptor.target_format = PreviewContentFormat::media_file;
        descriptor.order = 500;
        descriptor.fluent_icon_glyph = 0xe946;
        if (!glance::components::copy_resource_key(
                L"Shortcut.Tooltip",
                descriptor.tooltip_key))
        {
            return FALSE;
        }
        descriptors[0] = descriptor;
        return TRUE;
    }

    StatusBarShortcutState WINAPI query_shortcut_state(
        const wchar_t* requested_shortcut_id,
        const wchar_t* path,
        PreviewContentKind kind,
        PreviewContentFormat format) noexcept
    {
        if (requested_shortcut_id == nullptr || path == nullptr ||
            wcscmp(requested_shortcut_id, shortcut_id) != 0 ||
            kind != PreviewContentKind::media ||
            format != PreviewContentFormat::media_file)
        {
            return StatusBarShortcutState::hidden;
        }
        return available()
            ? StatusBarShortcutState::ready
            : StatusBarShortcutState::setup_required;
    }

    BOOL WINAPI activate_shortcut(
        const wchar_t* requested_shortcut_id,
        const wchar_t* path,
        BOOL requested_checked,
        StatusBarShortcutActivationResult* result) noexcept
    {
        if (requested_shortcut_id == nullptr || path == nullptr || result == nullptr ||
            result->size < sizeof(StatusBarShortcutActivationResult) ||
            wcscmp(requested_shortcut_id, shortcut_id) != 0)
        {
            return FALSE;
        }
        StatusBarShortcutActivationResult activation;
        if (available())
        {
            activation.activation = StatusBarShortcutActivation::toggle_hover_info;
            activation.checked = requested_checked;
            wcscpy_s(activation.hover_info_id, hover_info_id);
            if (!glance::components::copy_resource_key(
                    L"Preview.Loading",
                    activation.loading_text_key))
            {
                return FALSE;
            }
        }
        else
        {
            activation.activation = StatusBarShortcutActivation::request_dependency;
            wcscpy_s(activation.dependency_id, L"ffprobe");
        }
        *result = activation;
        return TRUE;
    }

    PrepareStatus WINAPI query_hover_info(
        const wchar_t* requested_hover_info_id,
        const wchar_t* path,
        const InformationPanelSink* sink) noexcept
    {
        if (requested_hover_info_id == nullptr || path == nullptr || sink == nullptr ||
            sink->size < sizeof(InformationPanelSink) || sink->append == nullptr ||
            wcscmp(requested_hover_info_id, hover_info_id) != 0)
        {
            return PrepareStatus::failed;
        }
        if (!available())
        {
            return PrepareStatus::unavailable;
        }
        return glance::components::media_info::query_media_info(
            *dependencies,
            path,
            *sink);
    }

    PrepareStatus WINAPI query_shortcut_data(
        const wchar_t* requested_shortcut_id,
        const wchar_t* path,
        const HoverInfoTextSink* sink) noexcept
    {
        if (requested_shortcut_id == nullptr || path == nullptr || sink == nullptr ||
            sink->size < sizeof(HoverInfoTextSink) || sink->append == nullptr ||
            wcscmp(requested_shortcut_id, shortcut_id) != 0)
        {
            return PrepareStatus::failed;
        }
        if (!available())
        {
            return PrepareStatus::unavailable;
        }
        const auto json = glance::components::media_info::query_media_json(
            *dependencies,
            path,
            *sink);
        if (json.empty())
        {
            return sink->is_cancelled != nullptr && sink->is_cancelled(sink->context)
                ? PrepareStatus::cancelled
                : PrepareStatus::failed;
        }
        if (json.size() > std::numeric_limits<std::uint32_t>::max() ||
            !sink->append(
                sink->context,
                json.c_str(),
                static_cast<std::uint32_t>(json.size())))
        {
            return PrepareStatus::failed;
        }
        return PrepareStatus::success;
    }


    const InformationProviderApi information_api{
        .query_info = query_hover_info,
        .query_json = query_shortcut_data };
    const StatusBarShortcutApi shortcut_api{
        .enumerate_shortcuts = enumerate_shortcuts,
        .query_state = query_shortcut_state,
        .activate = activate_shortcut };

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
        if (IsEqualGUID(*interface_id, information_provider_api_id) &&
            minimum_version <= information_provider_api_version)
        {
            *interface_pointer = const_cast<InformationProviderApi*>(&information_api);
            return TRUE;
        }
        if (IsEqualGUID(*interface_id, status_bar_shortcut_api_id) &&
            minimum_version <= status_bar_shortcut_api_version)
        {
            *interface_pointer = const_cast<StatusBarShortcutApi*>(&shortcut_api);
            return TRUE;
        }
        return FALSE;
    }

    void WINAPI shutdown() noexcept
    {
        dependencies = nullptr;
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI GlanceComponentGetApi(
    std::uint32_t host_abi,
    glance::contracts::components::ComponentApi* api) noexcept
{
    using namespace glance::contracts::components;
    if (host_abi != abi_version || api == nullptr || api->size < sizeof(ComponentApi))
    {
        return FALSE;
    }
    ComponentApi result;
    result.initialize = initialize;
    result.query_status = query_status;
    result.query_interface = query_interface;
    result.shutdown = shutdown;
    *api = result;
    return TRUE;
}

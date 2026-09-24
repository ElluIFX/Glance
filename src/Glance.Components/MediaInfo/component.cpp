#include "pch.h"

#include "glance/contracts/component_api.h"
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
    constexpr wchar_t action_id[] = L"prepare-ffprobe";
    constexpr wchar_t archive_url[] =
        L"https://www.gyan.dev/ffmpeg/builds/packages/ffmpeg-8.1.2-essentials_build.7z";
    constexpr wchar_t archive_file_name[] = L"ffmpeg-8.1.2-essentials_build.7z";
    constexpr wchar_t archive_sha256[] =
        L"e25b682664025d49034c981afb4bae36238a40f29a3cc1c713ad9a8b5b3528f6";
    constexpr std::uint64_t archive_size = 33876939;

    std::mutex state_mutex;
    std::filesystem::path ffprobe_path;

    bool available() noexcept
    {
        std::scoped_lock lock(state_mutex);
        return !ffprobe_path.empty();
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

        {
            std::scoped_lock lock(state_mutex);
            ffprobe_path = glance::components::media_info::find_ffprobe();
        }
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
            activation.activation = StatusBarShortcutActivation::request_component_action;
            wcscpy_s(activation.component_action_id, action_id);
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
        std::filesystem::path executable;
        {
            std::scoped_lock lock(state_mutex);
            executable = ffprobe_path;
        }
        if (executable.empty())
        {
            return PrepareStatus::unavailable;
        }
        return glance::components::media_info::query_media_info(
            executable,
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
        std::filesystem::path executable;
        {
            std::scoped_lock lock(state_mutex);
            executable = ffprobe_path;
        }
        if (executable.empty())
        {
            return PrepareStatus::unavailable;
        }
        const auto json = glance::components::media_info::query_media_json(
            executable,
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

    BOOL WINAPI enumerate_actions(
        ComponentManagementActionDescriptor* descriptors,
        std::uint32_t capacity,
        std::uint32_t* count) noexcept
    {
        if (count == nullptr)
        {
            return FALSE;
        }
        *count = available() ? 0 : 1;
        if (*count == 0 || descriptors == nullptr || capacity == 0)
        {
            return TRUE;
        }
        if (capacity < 1 || descriptors[0].size < sizeof(ComponentManagementActionDescriptor))
        {
            return FALSE;
        }
        ComponentManagementActionDescriptor descriptor;
        wcscpy_s(descriptor.action_id, action_id);
        if (!glance::components::copy_resource_key(
                L"Action.Button",
                descriptor.button_text_key) ||
            !glance::components::copy_resource_key(
                L"Action.ConfirmationTitle",
                descriptor.confirmation_title_key) ||
            !glance::components::copy_resource_key(
                L"Action.ConfirmationMessage",
                descriptor.confirmation_message_key) ||
            !glance::components::copy_resource_key(
                L"Action.ConfirmationButton",
                descriptor.confirmation_button_key) ||
            !glance::components::copy_resource_key(
                L"Action.DownloadTitle",
                descriptor.download_title_key) ||
            !glance::components::copy_resource_key(
                L"Action.DownloadMessage",
                descriptor.download_message_key) ||
            !glance::components::copy_resource_key(
                L"Action.PreparingTitle",
                descriptor.preparing_title_key) ||
            !glance::components::copy_resource_key(
                L"Action.PreparingMessage",
                descriptor.preparing_message_key) ||
            !glance::components::copy_resource_key(
                L"Action.CompletedTitle",
                descriptor.completed_title_key) ||
            !glance::components::copy_resource_key(
                L"Action.CompletedMessage",
                descriptor.completed_message_key))
        {
            return FALSE;
        }
        descriptors[0] = descriptor;
        return TRUE;
    }

    BOOL WINAPI prepare_action(
        const wchar_t* requested_action_id,
        ComponentDownloadRequest* request) noexcept
    {
        if (requested_action_id == nullptr || request == nullptr ||
            request->size < sizeof(ComponentDownloadRequest) ||
            wcscmp(requested_action_id, action_id) != 0 || available())
        {
            return FALSE;
        }
        ComponentDownloadRequest download;
        wcscpy_s(download.url, archive_url);
        wcscpy_s(download.file_name, archive_file_name);
        wcscpy_s(download.sha256, archive_sha256);
        download.expected_size = archive_size;
        *request = download;
        return TRUE;
    }

    BOOL WINAPI complete_action(
        const wchar_t* requested_action_id,
        const wchar_t* downloaded_path,
        const wchar_t* component_storage_path,
        ComponentManagementActionResult* result) noexcept
    {
        if (requested_action_id == nullptr || downloaded_path == nullptr ||
            component_storage_path == nullptr || result == nullptr ||
            result->size < sizeof(ComponentManagementActionResult) ||
            wcscmp(requested_action_id, action_id) != 0)
        {
            return FALSE;
        }
        std::wstring error_key;
        ComponentManagementActionResult action_result;
        action_result.succeeded = glance::components::media_info::install_ffprobe(
            downloaded_path,
            component_storage_path,
            error_key);
        if (action_result.succeeded)
        {
            const auto installed = glance::components::media_info::find_ffprobe();
            if (installed.empty())
            {
                action_result.succeeded = FALSE;
                error_key = L"Action.InstallFailed";
            }
            else
            {
                std::scoped_lock lock(state_mutex);
                ffprobe_path = installed;
            }
        }
        if (!action_result.succeeded &&
            !glance::components::copy_resource_key(
                error_key,
                action_result.detail_key))
        {
            return FALSE;
        }
        *result = action_result;
        return TRUE;
    }

    const InformationProviderApi information_api{
        .query_info = query_hover_info,
        .query_json = query_shortcut_data };
    const StatusBarShortcutApi shortcut_api{
        .enumerate_shortcuts = enumerate_shortcuts,
        .query_state = query_shortcut_state,
        .activate = activate_shortcut };
    const ComponentManagementActionApi management_action_api{
        .enumerate_actions = enumerate_actions,
        .prepare_action = prepare_action,
        .complete_action = complete_action };

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
        if (IsEqualGUID(*interface_id, component_management_action_api_id) &&
            minimum_version <= component_management_action_api_version)
        {
            *interface_pointer =
                const_cast<ComponentManagementActionApi*>(&management_action_api);
            return TRUE;
        }
        return FALSE;
    }

    void WINAPI shutdown() noexcept
    {
        {
            std::scoped_lock lock(state_mutex);
            ffprobe_path.clear();
        }
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

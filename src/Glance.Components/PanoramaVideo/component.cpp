#include "pch.h"
#include "../Common/preview_cancellation.h"

#include "glance/contracts/component_api.h"
#include "../Common/component_text.h"
#include "../../version.h"

#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <ranges>
#include <string>
#include <string_view>

namespace
{
    using namespace glance::contracts::components;

    constexpr std::array panorama_extensions{ L".insv", L".osv" };
    constexpr wchar_t display_name_key[] = L"Component.DisplayName";
    constexpr wchar_t status_available_key[] = L"Status.Available";
    constexpr wchar_t status_unavailable_key[] = L"Status.Unavailable";
    constexpr wchar_t loading_key[] = L"Preview.Loading";
    constexpr wchar_t projection_shortcut_id[] = L"projection-view";
    constexpr wchar_t projection_tooltip_key[] = L"Shortcut.ProjectionTooltip";
    constexpr wchar_t settings_group_key[] = L"Settings.GroupTitle";
    constexpr wchar_t settings_insv_row_key[] = L"Settings.InsvRow";
    constexpr wchar_t settings_osv_row_key[] = L"Settings.OsvRow";
    constexpr wchar_t settings_view_angle_key[] = L"Settings.ViewAngle";
    constexpr wchar_t settings_overlap_key[] = L"Settings.Overlap";

    struct PanoramaSettingDefinition
    {
        const wchar_t* id;
        const wchar_t* row_id;
        const wchar_t* row_title_key;
        const wchar_t* label_key;
        std::int64_t default_value;
        std::int64_t minimum_value;
        std::int64_t maximum_value;
    };

    constexpr std::array panorama_settings{
        PanoramaSettingDefinition{
            L"insv-view-angle", L"insv", settings_insv_row_key,
            settings_view_angle_key, 1930, 1800, 2200 },
        PanoramaSettingDefinition{
            L"insv-overlap", L"insv", settings_insv_row_key,
            settings_overlap_key, 60, 0, 200 },
        PanoramaSettingDefinition{
            L"osv-view-angle", L"osv", settings_osv_row_key,
            settings_view_angle_key, 1930, 1800, 2200 },
        PanoramaSettingDefinition{
            L"osv-overlap", L"osv", settings_osv_row_key,
            settings_overlap_key, 60, 0, 200 },
    };

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

    std::wstring lowercase(std::wstring value)
    {
        std::ranges::transform(value, value.begin(), [](wchar_t character) {
            return std::towlower(character);
        });
        return value;
    }

    bool is_panorama_extension(const std::filesystem::path& path) noexcept
    {
        const auto extension = lowercase(path.extension().wstring());
        return std::ranges::find(panorama_extensions, extension) !=
            panorama_extensions.end();
    }

    bool renderer_available() noexcept
    {
        D3D_FEATURE_LEVEL feature_level{};
        ID3D11Device* device{};
        ID3D11DeviceContext* context{};
        const HRESULT d3d_status = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &device,
            &feature_level,
            &context);
        if (context != nullptr)
        {
            context->Release();
        }
        if (device != nullptr)
        {
            device->Release();
        }
        if (FAILED(d3d_status))
        {
            return false;
        }
        static_cast<void>(feature_level);

        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
        {
            return false;
        }
        MFT_REGISTER_TYPE_INFO input{
            .guidMajorType = MFMediaType_Video,
            .guidSubtype = MFVideoFormat_HEVC };
        IMFActivate** activations{};
        UINT32 count{};
        const HRESULT media_status = MFTEnumEx(
            MFT_CATEGORY_VIDEO_DECODER,
            MFT_ENUM_FLAG_ALL,
            &input,
            nullptr,
            &activations,
            &count);
        if (activations != nullptr)
        {
            for (UINT32 index = 0; index < count; ++index)
            {
                activations[index]->Release();
            }
            CoTaskMemFree(activations);
        }
        MFShutdown();
        return SUCCEEDED(media_status) && count != 0;
    }

    BOOL WINAPI query_host(PreviewHostProtocol protocol, RendererHostDescriptor* descriptor) noexcept
    {
        if (protocol != PreviewHostProtocol::native_media)
            return FALSE;
        if (descriptor == nullptr || descriptor->size < sizeof(*descriptor))
        {
            return FALSE;
        }
        std::error_code error;
        if (!std::filesystem::is_regular_file(
                component_directory() / L"Glance.PanoramaVideoHost.exe",
                error))
        {
            return FALSE;
        }
        RendererHostDescriptor result;
        wcscpy_s(result.host_executable, L"Glance.PanoramaVideoHost.exe");
        *descriptor = result;
        return TRUE;
    }

    const HostRendererApi native_media_api{ .query_host = query_host };

    BOOL WINAPI enumerate_settings(
        ComponentSettingDescriptor* descriptors,
        std::uint32_t capacity,
        std::uint32_t* count) noexcept
    {
        if (count == nullptr)
        {
            return FALSE;
        }
        *count = static_cast<std::uint32_t>(panorama_settings.size());
        if (descriptors == nullptr ||
            capacity < static_cast<std::uint32_t>(panorama_settings.size()))
        {
            return descriptors == nullptr && capacity == 0;
        }
        for (std::size_t index = 0; index < panorama_settings.size(); ++index)
        {
            const auto& definition = panorama_settings[index];
            ComponentSettingDescriptor setting;
            wcscpy_s(setting.setting_id, definition.id);
            setting.page = ComponentSettingPage::media_preview;
            wcscpy_s(setting.group_id, L"panorama-preview");
            wcscpy_s(setting.row_id, definition.row_id);
            setting.kind = ComponentSettingKind::number;
            setting.default_value = definition.default_value;
            setting.minimum_value = definition.minimum_value;
            setting.maximum_value = definition.maximum_value;
            setting.small_change = 1;
            setting.decimal_places = 1;
            setting.group_order = 1000;
            setting.setting_order = static_cast<std::uint32_t>(index);
            if (!glance::components::copy_resource_key(
                    settings_group_key,
                    setting.group_title_key) ||
                !glance::components::copy_resource_key(
                    definition.row_title_key,
                    setting.row_title_key) ||
                !glance::components::copy_resource_key(
                    definition.label_key,
                    setting.label_key))
            {
                return FALSE;
            }
            descriptors[index] = setting;
        }
        return TRUE;
    }

    const SettingsContributionApi settings_api{
        .enumerate_settings = enumerate_settings };

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
        wcscpy_s(descriptor.shortcut_id, projection_shortcut_id);
        descriptor.target_kind = PreviewContentKind::media;
        descriptor.target_format = PreviewContentFormat::media_file;
        descriptor.order = 100;
        descriptor.fluent_icon_glyph = 0xe774;
        descriptor.initially_checked = TRUE;
        if (!glance::components::copy_resource_key(
                projection_tooltip_key,
                descriptor.tooltip_key))
        {
            return FALSE;
        }
        descriptors[0] = descriptor;
        return TRUE;
    }

    StatusBarShortcutState WINAPI query_shortcut_state(
        const wchar_t* shortcut_id,
        const wchar_t* path,
        PreviewContentKind kind,
        PreviewContentFormat format) noexcept
    {
        return shortcut_id != nullptr && path != nullptr &&
                wcscmp(shortcut_id, projection_shortcut_id) == 0 &&
                kind == PreviewContentKind::media &&
                format == PreviewContentFormat::media_file &&
                is_panorama_extension(std::filesystem::path(path))
            ? StatusBarShortcutState::ready
            : StatusBarShortcutState::hidden;
    }

    BOOL WINAPI activate_shortcut(
        const wchar_t* shortcut_id,
        const wchar_t* path,
        BOOL requested_checked,
        StatusBarShortcutActivationResult* result) noexcept
    {
        if (shortcut_id == nullptr || path == nullptr || result == nullptr ||
            result->size < sizeof(StatusBarShortcutActivationResult) ||
            wcscmp(shortcut_id, projection_shortcut_id) != 0 ||
            !is_panorama_extension(std::filesystem::path(path)))
        {
            return FALSE;
        }
        StatusBarShortcutActivationResult activation;
        activation.activation = StatusBarShortcutActivation::set_native_media_view_mode;
        activation.checked = requested_checked;
        *result = activation;
        return TRUE;
    }

    const StatusBarShortcutApi shortcut_api{
        .enumerate_shortcuts = enumerate_shortcuts,
        .query_state = query_shortcut_state,
        .activate = activate_shortcut };

    BOOL WINAPI initialize(
        const ComponentRegistrar* registrar,
        ComponentRegistration* registration) noexcept
    {
        if (registrar == nullptr || registrar->size < sizeof(*registrar) ||
            registrar->register_extension == nullptr ||
            registrar->register_renderer == nullptr || registration == nullptr ||
            registration->size < sizeof(*registration))
        {
            return FALSE;
        }
        for (const auto* extension : panorama_extensions)
        {
            if (!registrar->register_extension(registrar->context, extension, GalleryMediaKind::video))
            {
                return FALSE;
            }
        }
        if (!registrar->register_renderer(
                registrar->context,
                PreviewContentKind::media,
                PreviewContentFormat::media_file,
                &host_renderer_api_id,
                host_renderer_api_version))
        {
            return FALSE;
        }
        ComponentRegistration result;
        wcscpy_s(result.component_id, L"panorama-video");
        wcscpy_s(result.target_app_version, GLANCE_VERSION_WSTRING);
        wcscpy_s(result.resource_path, L"resources.pri");
        result.preferred_kind = PreviewContentKind::media;
        result.preferred_format = PreviewContentFormat::media_file;
        *registration = result;
        return TRUE;
    }

    BOOL WINAPI query_status(ComponentStatusResult* result) noexcept
    {
        if (result == nullptr || result->size < sizeof(*result))
        {
            return FALSE;
        }
        const bool available = renderer_available();
        ComponentStatusResult status;
        status.severity = available ? HealthSeverity::healthy : HealthSeverity::error;
        if (!glance::components::copy_resource_key(
                display_name_key,
                status.display_name_key) ||
            !glance::components::copy_resource_key(
                available ? status_available_key : status_unavailable_key,
                status.detail_key))
        {
            return FALSE;
        }
        *result = status;
        return TRUE;
    }

    BOOL WINAPI query_loading_text(
        const wchar_t* path,
        ComponentLoadingTextResult* result) noexcept
    {
        if (path == nullptr || result == nullptr || result->size < sizeof(*result))
        {
            return FALSE;
        }
        ComponentLoadingTextResult loading;
        if (!glance::components::copy_resource_key(loading_key, loading.key))
        {
            return FALSE;
        }
        *result = loading;
        return TRUE;
    }

    BOOL WINAPI can_preview(const wchar_t* path) noexcept
    {
        return path != nullptr && is_panorama_extension(std::filesystem::path(path));
    }

    PrepareStatus WINAPI prepare_preview(
        const wchar_t* path,
        PreparedPreview* preview) noexcept
    {
        if (path == nullptr || preview == nullptr || preview->size < sizeof(*preview))
        {
            return PrepareStatus::failed;
        }
        try
        {
            const std::filesystem::path source(path);
            std::error_code error;
            if (!source.is_absolute() || !is_panorama_extension(source) ||
                !std::filesystem::is_regular_file(source, error) || error)
            {
                return PrepareStatus::unavailable;
            }
            const auto value = source.wstring();
            if (value.size() + 1 > preview_path_capacity)
            {
                return PrepareStatus::failed;
            }
            PreparedPreview prepared;
            prepared.kind = PreviewContentKind::media;
            prepared.format = PreviewContentFormat::media_file;
            std::copy(value.begin(), value.end(), prepared.path);
            prepared.path[value.size()] = L'\0';
            *preview = prepared;
            return PrepareStatus::success;
        }
        catch (...)
        {
            return PrepareStatus::failed;
        }
    }

    void WINAPI release_preview(std::uint64_t) noexcept
    {
    }

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
        if (IsEqualGUID(*interface_id, host_renderer_api_id) &&
            minimum_version <= host_renderer_api_version)
        {
            *interface_pointer = const_cast<HostRendererApi*>(&native_media_api);
            return TRUE;
        }
        if (IsEqualGUID(*interface_id, settings_contribution_api_id) &&
            minimum_version <= settings_contribution_api_version)
        {
            *interface_pointer = const_cast<SettingsContributionApi*>(&settings_api);
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
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI GlanceComponentGetApi(
    std::uint32_t host_abi,
    glance::contracts::components::ComponentApi* api) noexcept
{
    using namespace glance::contracts::components;
    if (host_abi != abi_version || api == nullptr || api->size < sizeof(*api))
    {
        return FALSE;
    }
    ComponentApi result;
    result.initialize = initialize;
    result.query_status = query_status;
    result.query_loading_text = query_loading_text;
    result.can_preview = can_preview;
    result.prepare_preview = glance::components::prepare_preview_callback<prepare_preview>;
    result.release_preview = release_preview;
    result.query_interface = query_interface;
    result.shutdown = shutdown;
    *api = result;
    return TRUE;
}

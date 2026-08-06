#include "pch.h"

#include "glance/contracts/component_api.h"
#include "../Common/component_localization.h"
#include "../../version.h"

#include <algorithm>
#include <array>
#include <cwchar>
#include <filesystem>
#include <string>

namespace
{
    using namespace glance::contracts::components;

    constexpr wchar_t display_name_key[] = L"Component.DisplayName";
    constexpr wchar_t status_available_key[] = L"Status.Available";
    constexpr wchar_t status_unavailable_key[] = L"Status.Unavailable";
    constexpr wchar_t loading_key[] = L"Preview.Loading";
    constexpr wchar_t settings_group_key[] = L"Settings.GroupTitle";
    constexpr wchar_t settings_label_key[] = L"Settings.RenderResolution.Label";
    constexpr wchar_t settings_description_key[] = L"Settings.RenderResolution.Description";
    constexpr std::array setting_option_keys{
        L"Settings.RenderResolution.1024",
        L"Settings.RenderResolution.2048",
        L"Settings.RenderResolution.4096",
        L"Settings.RenderResolution.8192" };
    constexpr std::array<std::int64_t, 4> setting_option_values{
        1024, 2048, 4096, 8192 };

    glance::components::ComponentResourceStore component_resources;

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
            !component_resources.initialize() ||
            !registrar->register_extension(registrar->context, L".pdf") ||
            !registrar->register_renderer(
                registrar->context,
                PreviewContentKind::document,
                PreviewContentFormat::pdf,
                &paged_document_renderer_api_id,
                paged_document_renderer_api_version))
        {
            return FALSE;
        }

        ComponentRegistration result;
        wcscpy_s(result.component_id, L"pdf");
        wcscpy_s(result.target_app_version, GLANCE_VERSION_WSTRING);
        result.preferred_kind = PreviewContentKind::document;
        result.preferred_format = PreviewContentFormat::pdf;
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
        const auto directory = component_directory();
        std::error_code error;
        const bool available =
            std::filesystem::is_regular_file(directory / L"Glance.PdfHost.exe", error) &&
            std::filesystem::is_regular_file(directory / L"pdfium.dll", error);
        ComponentStatusResult status;
        status.severity = available ? HealthSeverity::healthy : HealthSeverity::error;
        if (!localize(display_name_key, language_tag, status.display_name) ||
            !localize(
                available ? status_available_key : status_unavailable_key,
                language_tag,
                status.detail))
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
        ComponentLoadingTextResult text;
        if (!localize(loading_key, language_tag, text.text))
        {
            return FALSE;
        }
        *result = text;
        return TRUE;
    }

    BOOL WINAPI can_preview(const wchar_t* path) noexcept
    {
        return path != nullptr &&
            _wcsicmp(std::filesystem::path(path).extension().c_str(), L".pdf") == 0;
    }

    PrepareStatus WINAPI prepare_preview(
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
                value.size() + 1 > preview_path_capacity)
            {
                return PrepareStatus::unavailable;
            }
            PreparedPreview prepared;
            prepared.kind = PreviewContentKind::document;
            prepared.format = PreviewContentFormat::pdf;
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

    BOOL WINAPI query_host(PagedDocumentHostDescriptor* descriptor) noexcept
    {
        if (descriptor == nullptr || descriptor->size < sizeof(PagedDocumentHostDescriptor))
        {
            return FALSE;
        }
        const auto directory = component_directory();
        std::error_code error;
        if (!std::filesystem::is_regular_file(directory / L"Glance.PdfHost.exe", error) ||
            !std::filesystem::is_regular_file(directory / L"pdfium.dll", error))
        {
            return FALSE;
        }
        PagedDocumentHostDescriptor result;
        wcscpy_s(result.host_executable, L"Glance.PdfHost.exe");
        *descriptor = result;
        return TRUE;
    }

    BOOL WINAPI enumerate_settings(
        const wchar_t* language_tag,
        ComponentSettingDescriptor* descriptors,
        std::uint32_t capacity,
        std::uint32_t* count) noexcept
    {
        if (count == nullptr)
        {
            return FALSE;
        }
        *count = 1;
        if (descriptors == nullptr || capacity < 1)
        {
            return descriptors == nullptr && capacity == 0;
        }

        ComponentSettingDescriptor setting;
        wcscpy_s(setting.setting_id, L"render-dimension");
        setting.page = ComponentSettingPage::document_preview;
        wcscpy_s(setting.group_id, L"rich-document");
        setting.kind = ComponentSettingKind::choice;
        setting.default_value = 4096;
        setting.group_order = 1000;
        setting.setting_order = 0;
        setting.option_count = static_cast<std::uint32_t>(setting_option_values.size());
        if (!localize(settings_group_key, language_tag, setting.group_title) ||
            !localize(settings_label_key, language_tag, setting.label) ||
            !localize(settings_description_key, language_tag, setting.description))
        {
            return FALSE;
        }
        for (std::size_t index = 0; index < setting_option_values.size(); ++index)
        {
            setting.options[index].value = setting_option_values[index];
            if (!localize(
                    setting_option_keys[index],
                    language_tag,
                    setting.options[index].text))
            {
                return FALSE;
            }
        }
        descriptors[0] = setting;
        return TRUE;
    }

    const PagedDocumentRendererApi paged_document_api{
        .query_host = query_host };
    const SettingsContributionApi settings_api{
        .enumerate_settings = enumerate_settings };

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
        if (IsEqualGUID(*interface_id, paged_document_renderer_api_id) &&
            minimum_version <= paged_document_renderer_api_version)
        {
            *interface_pointer = const_cast<PagedDocumentRendererApi*>(&paged_document_api);
            return TRUE;
        }
        if (IsEqualGUID(*interface_id, settings_contribution_api_id) &&
            minimum_version <= settings_contribution_api_version)
        {
            *interface_pointer = const_cast<SettingsContributionApi*>(&settings_api);
            return TRUE;
        }
        return FALSE;
    }

    void WINAPI shutdown() noexcept
    {
        component_resources.shutdown();
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
    result.query_loading_text = query_loading_text;
    result.can_preview = can_preview;
    result.prepare_preview = prepare_preview;
    result.release_preview = release_preview;
    result.query_interface = query_interface;
    result.shutdown = shutdown;
    *api = result;
    return TRUE;
}

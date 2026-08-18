#include "pch.h"

#include "adobe_preview_service.h"
#include "../Common/component_localization.h"
#include "../../version.h"
#include "glance/contracts/component_api.h"

#include <array>
#include <cwchar>

namespace
{
    using namespace glance::contracts::components;
    constexpr std::array adobe_extensions{ L".psd", L".psb", L".ai" };
    constexpr wchar_t display_name_key[] = L"Component.DisplayName";
    constexpr wchar_t status_key[] = L"Status.BuiltIn";
    constexpr wchar_t loading_key[] = L"Preview.Loading";
    constexpr wchar_t refining_key[] = L"Preview.Refining";
    constexpr wchar_t refinement_failed_key[] = L"Preview.RefinementFailed";
    constexpr wchar_t low_resolution_only_key[] = L"Preview.LowResolutionOnly";
    constexpr wchar_t unavailable_key[] = L"Preview.Unavailable";
    constexpr wchar_t failed_key[] = L"Preview.Failed";

    glance::components::ComponentResourceStore component_resources;

    template <std::size_t Size>
    bool localize(
        const wchar_t* key,
        const wchar_t* language_tag,
        wchar_t (&destination)[Size]) noexcept
    {
        return component_resources.copy(key, language_tag, destination, Size);
    }

    BOOL WINAPI initialize(
        const ComponentRegistrar* registrar,
        ComponentRegistration* registration) noexcept
    {
        if (registrar == nullptr ||
            registrar->size < sizeof(ComponentRegistrar) ||
            registrar->register_extension == nullptr ||
            registration == nullptr ||
            registration->size < sizeof(ComponentRegistration) ||
            !component_resources.initialize())
        {
            return FALSE;
        }

        glance::components::adobe::initialize();
        for (const auto* extension : adobe_extensions)
        {
            if (!registrar->register_extension(registrar->context, extension))
            {
                return FALSE;
            }
        }

        ComponentRegistration result;
        wcscpy_s(result.component_id, L"adobe");
        wcscpy_s(result.target_app_version, GLANCE_VERSION_WSTRING);
        result.preferred_kind = PreviewContentKind::image;
        result.preferred_format = PreviewContentFormat::image_file;
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

        ComponentStatusResult status;
        status.severity = HealthSeverity::healthy;
        if (!localize(display_name_key, language_tag, status.display_name) ||
            !localize(status_key, language_tag, status.detail))
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
        if (path == nullptr ||
            result == nullptr ||
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
        return path != nullptr &&
            glance::components::adobe::can_preview(path);
    }

    PrepareStatus copy_preview_result(
        const glance::components::adobe::PreviewResult& result,
        const wchar_t* language_tag,
        PreparedPreview* preview) noexcept
    {
        if (result.status != PrepareStatus::success)
        {
            localize(
                result.status == PrepareStatus::unavailable
                    ? unavailable_key
                    : failed_key,
                language_tag,
                preview->error_detail);
            return result.status;
        }
        const auto output_path = result.path.wstring();
        if (output_path.size() + 1 > preview_path_capacity)
        {
            glance::components::adobe::release_preview(result.lease_token);
            localize(failed_key, language_tag, preview->error_detail);
            return PrepareStatus::failed;
        }

        PreparedPreview prepared;
        prepared.kind = result.kind;
        prepared.format = result.format;
        prepared.lease_token = result.lease_token;
        wcscpy_s(prepared.path, output_path.c_str());
        *preview = prepared;
        return PrepareStatus::success;
    }

    PrepareStatus WINAPI prepare_preview_with_options(
        const wchar_t* path,
        const wchar_t* language_tag,
        const PreviewPreparationOptions* options,
        PreparedPreview* preview) noexcept
    {
        if (path == nullptr ||
            options == nullptr ||
            options->size < sizeof(PreviewPreparationOptions) ||
            preview == nullptr ||
            preview->size < sizeof(PreparedPreview))
        {
            return PrepareStatus::failed;
        }
        return copy_preview_result(
            glance::components::adobe::prepare_preview(
                path,
                options->maximum_dimension),
            language_tag,
            preview);
    }

    PrepareStatus WINAPI prepare_preview(
        const wchar_t* path,
        const wchar_t* language_tag,
        PreparedPreview* preview) noexcept
    {
        PreviewPreparationOptions options;
        return prepare_preview_with_options(
            path,
            language_tag,
            &options,
            preview);
    }

    BOOL WINAPI can_refine(std::uint64_t lease_token) noexcept
    {
        return glance::components::adobe::can_refine(lease_token);
    }

    BOOL WINAPI query_refinement_text(
        std::uint64_t lease_token,
        const wchar_t* language_tag,
        ComponentLoadingTextResult* result) noexcept
    {
        if (!glance::components::adobe::can_refine(lease_token) ||
            result == nullptr ||
            result->size < sizeof(ComponentLoadingTextResult))
        {
            return FALSE;
        }
        ComponentLoadingTextResult text;
        if (!localize(refining_key, language_tag, text.text))
        {
            return FALSE;
        }
        *result = text;
        return TRUE;
    }

    PrepareStatus WINAPI prepare_refined_preview(
        std::uint64_t lease_token,
        const wchar_t* language_tag,
        const PreviewPreparationOptions* options,
        PreparedPreview* preview) noexcept
    {
        if (options == nullptr ||
            options->size < sizeof(PreviewPreparationOptions) ||
            preview == nullptr ||
            preview->size < sizeof(PreparedPreview))
        {
            return PrepareStatus::failed;
        }
        const auto result =
            glance::components::adobe::prepare_refined_preview(
                lease_token,
                options->maximum_dimension);
        const auto status =
            copy_preview_result(result, language_tag, preview);
        if (status != PrepareStatus::success)
        {
            localize(
                refinement_failed_key,
                language_tag,
                preview->error_detail);
        }
        return status;
    }

    void WINAPI release_preview(std::uint64_t lease_token) noexcept
    {
        glance::components::adobe::release_preview(lease_token);
    }

    ConfigurablePreviewApi configurable_preview_api{
        .prepare_preview = prepare_preview_with_options };
    ProgressivePreviewApi progressive_preview_api{
        .can_refine = can_refine,
        .query_refinement_text = query_refinement_text,
        .prepare_refined_preview = prepare_refined_preview };

    BOOL WINAPI query_preview_notice(
        std::uint64_t lease_token,
        const wchar_t* language_tag,
        PreviewNoticeResult* result) noexcept
    {
        if (!glance::components::adobe::is_low_resolution_only(lease_token) ||
            result == nullptr ||
            result->size < sizeof(PreviewNoticeResult))
        {
            return FALSE;
        }
        PreviewNoticeResult notice;
        if (!localize(
                low_resolution_only_key,
                language_tag,
                notice.text))
        {
            return FALSE;
        }
        *result = notice;
        return TRUE;
    }

    PreviewNoticeApi preview_notice_api{
        .query_preview_notice = query_preview_notice };

    GalleryMediaKind WINAPI classify_gallery_extension(const wchar_t* extension) noexcept
    {
        if (extension != nullptr &&
            (_wcsicmp(extension, L".psd") == 0 || _wcsicmp(extension, L".psb") == 0))
        {
            return GalleryMediaKind::image;
        }
        return GalleryMediaKind::none;
    }

    GalleryMediaApi gallery_media_api{
        .classify_extension = classify_gallery_extension };

    BOOL WINAPI query_interface(
        const GUID* interface_id,
        std::uint32_t minimum_version,
        void** interface_pointer) noexcept
    {
        if (interface_pointer == nullptr)
        {
            return FALSE;
        }
        *interface_pointer = nullptr;
        if (interface_id == nullptr || minimum_version > 1)
        {
            return FALSE;
        }
        if (IsEqualGUID(*interface_id, configurable_preview_api_id))
        {
            *interface_pointer = &configurable_preview_api;
            return TRUE;
        }
        if (IsEqualGUID(*interface_id, progressive_preview_api_id))
        {
            *interface_pointer = &progressive_preview_api;
            return TRUE;
        }
        if (IsEqualGUID(*interface_id, preview_notice_api_id))
        {
            *interface_pointer = &preview_notice_api;
            return TRUE;
        }
        if (IsEqualGUID(*interface_id, gallery_media_api_id))
        {
            *interface_pointer = &gallery_media_api;
            return TRUE;
        }
        return FALSE;
    }

    void WINAPI shutdown() noexcept
    {
        glance::components::adobe::shutdown();
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

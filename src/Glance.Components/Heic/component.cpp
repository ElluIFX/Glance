#include "pch.h"
#include "../Common/preview_cancellation.h"

#include "heic_preview_service.h"
#include "../Common/component_text.h"
#include "../../version.h"
#include "glance/contracts/component_api.h"

#include <array>
#include <cwchar>

namespace
{
    using namespace glance::contracts::components;
    constexpr std::array heic_extensions{ L".heic", L".heif", L".hif" };
    constexpr wchar_t display_name_key[] = L"Component.DisplayName";
    constexpr wchar_t status_key[] = L"Status.BuiltIn";
    constexpr wchar_t loading_key[] = L"Preview.Loading";
    constexpr wchar_t unavailable_key[] = L"Preview.Unavailable";
    constexpr wchar_t failed_key[] = L"Preview.Failed";
    constexpr std::uint32_t preview_dimension = 8192;

    BOOL WINAPI initialize(
        const ComponentRegistrar* registrar,
        ComponentRegistration* registration) noexcept
    {
        if (registrar == nullptr ||
            registrar->size < sizeof(ComponentRegistrar) ||
            registrar->register_extension == nullptr ||
            registration == nullptr ||
            registration->size < sizeof(ComponentRegistration))
        {
            return FALSE;
        }

        glance::components::heic::initialize();
        for (const auto* extension : heic_extensions)
        {
            if (!registrar->register_extension(registrar->context, extension, GalleryMediaKind::image))
            {
                return FALSE;
            }
        }

        ComponentRegistration result;
        wcscpy_s(result.component_id, L"heic");
        wcscpy_s(result.target_app_version, GLANCE_VERSION_WSTRING);
        wcscpy_s(result.resource_path, L"resources.pri");
        result.preferred_kind = PreviewContentKind::image;
        result.preferred_format = PreviewContentFormat::image_file;
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
        status.severity = HealthSeverity::healthy;
        if (!glance::components::copy_resource_key(
                display_name_key,
                status.display_name_key) ||
            !glance::components::copy_resource_key(status_key, status.detail_key))
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
        if (path == nullptr ||
            result == nullptr ||
            result->size < sizeof(ComponentLoadingTextResult))
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
        return path != nullptr &&
            glance::components::heic::can_preview(path);
    }

    PrepareStatus copy_preview_result(
        const glance::components::heic::PreviewResult& result,
        PreparedPreview* preview) noexcept
    {
        if (result.status != PrepareStatus::success)
        {
            glance::components::copy_resource_key(
                result.status == PrepareStatus::unavailable
                    ? unavailable_key
                    : failed_key,
                preview->error_key);
            return result.status;
        }
        const auto output_path = result.path.wstring();
        if (output_path.size() + 1 > preview_path_capacity)
        {
            glance::components::heic::release_preview(result.lease_token);
            glance::components::copy_resource_key(failed_key, preview->error_key);
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

    PrepareStatus WINAPI prepare_preview(
        const wchar_t* path,
        PreparedPreview* preview) noexcept
    {
        if (path == nullptr ||
            preview == nullptr ||
            preview->size < sizeof(PreparedPreview))
        {
            return PrepareStatus::failed;
        }
        return copy_preview_result(
            glance::components::heic::prepare_preview(path, preview_dimension),
            preview);
    }

    void WINAPI release_preview(std::uint64_t lease_token) noexcept
    {
        glance::components::heic::release_preview(lease_token);
    }

    PrepareStatus WINAPI prepare_preview_with_options(
        const wchar_t* path, const PreviewPreparationOptions* options, PreparedPreview* preview) noexcept
    {
        if (path == nullptr || preview == nullptr || preview->size < sizeof(PreparedPreview))
            return PrepareStatus::failed;
        const auto requested = options != nullptr ? options->maximum_dimension : preview_dimension;
        const auto dimension = requested <= 1024 ? 1024U : requested <= 2048 ? 2048U :
            requested <= 4096 ? 4096U : 8192U;
        return copy_preview_result(
            glance::components::heic::prepare_preview(path, dimension), preview);
    }

    BOOL WINAPI can_refine(std::uint64_t token) noexcept
    {
        return !glance::components::heic::refinement_source(token).empty();
    }

    BOOL WINAPI query_refinement_text(std::uint64_t, ComponentLoadingTextResult* result) noexcept
    {
        return result != nullptr && result->size >= sizeof(ComponentLoadingTextResult) &&
            glance::components::copy_resource_key(L"Preview.Refining", result->key);
    }

    PrepareStatus WINAPI prepare_refined_preview(
        std::uint64_t token, const PreviewPreparationOptions*, PreparedPreview* preview) noexcept
    {
        if (preview == nullptr || preview->size < sizeof(PreparedPreview))
            return PrepareStatus::failed;
        const auto source = glance::components::heic::refinement_source(token);
        if (source.empty()) return PrepareStatus::unavailable;
        return copy_preview_result(
            glance::components::heic::prepare_preview(source, preview_dimension), preview);
    }

    ProgressivePreviewApi progressive_api{
        .can_refine = can_refine,
        .query_refinement_text = query_refinement_text,
        .prepare_refined_preview = prepare_refined_preview };



    BOOL WINAPI query_image_metadata(
        std::uint64_t lease_token,
        const ImageMetadataSink* sink) noexcept
    {
        return glance::components::heic::query_metadata(lease_token, sink);
    }

    ImageMetadataApi image_metadata_api{
        .query_metadata = query_image_metadata };

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
        if (interface_id != nullptr && IsEqualGUID(*interface_id, cancellable_preview_api_id))
        {
            if (minimum_version > cancellable_preview_api_version) return FALSE;
            static auto api = [] {
                auto result = glance::components::cancellable_preview_api<
                    prepare_preview_with_options, prepare_refined_preview>();
                result.refine_on_zoom = TRUE;
                return result;
            }();
            *interface_pointer = &api;
            return TRUE;
        }
        if (interface_id != nullptr && IsEqualGUID(*interface_id, progressive_preview_api_id))
        {
            if (minimum_version > progressive_preview_api_version) return FALSE;
            *interface_pointer = &progressive_api;
            return TRUE;
        }
        if (interface_id == nullptr || minimum_version > 1)
        {
            return FALSE;
        }
        if (IsEqualGUID(*interface_id, image_metadata_api_id))
        {
            *interface_pointer = &image_metadata_api;
            return TRUE;
        }
        return FALSE;
    }

    void WINAPI shutdown() noexcept
    {
        glance::components::heic::shutdown();
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

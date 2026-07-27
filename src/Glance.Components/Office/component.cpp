#include "pch.h"

#include "glance/contracts/component_api.h"
#include "office_availability.h"
#include "office_pdf_service.h"
#include "../Common/component_localization.h"
#include "../../version.h"

#include <algorithm>
#include <array>
#include <cwchar>

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
    constexpr wchar_t office_unavailable_key[] = L"Preview.OfficeUnavailable";
    constexpr wchar_t prepare_failed_key[] = L"Preview.PrepareFailed";

    glance::components::ComponentResourceStore component_resources;

    template <std::size_t Size>
    bool localize(
        const wchar_t* key,
        const wchar_t* language_tag,
        wchar_t (&destination)[Size]) noexcept
    {
        return component_resources.copy(
            key,
            language_tag,
            destination,
            Size);
    }

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

        if (!component_resources.initialize())
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

        ComponentRegistration result;
        wcscpy_s(result.component_id, L"office");
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

        const auto mask = glance::app::office_available_components();
        ComponentStatusResult status;
        status.severity = mask == glance::app::office_all_components
            ? HealthSeverity::healthy
            : HealthSeverity::warning;
        status.code = 0;
        status.capability_mask = mask;
        if (!localize(display_name_key, language_tag, status.display_name))
        {
            return FALSE;
        }

        const wchar_t* detail_key{};
        if (mask == glance::app::office_all_components)
        {
            detail_key = status_available_key;
        }
        else if (mask == 0)
        {
            detail_key = status_unavailable_key;
        }
        else
        {
            detail_key = status_partial_key;
        }
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
        if (path == nullptr ||
            result == nullptr ||
            result->size < sizeof(ComponentLoadingTextResult))
        {
            return FALSE;
        }

        ComponentLoadingTextResult loading_text;
        if (!localize(loading_key, language_tag, loading_text.text))
        {
            return FALSE;
        }
        *result = loading_text;
        return TRUE;
    }

    BOOL WINAPI can_preview(const wchar_t* path) noexcept
    {
        return path != nullptr && glance::app::office_preview_available(path);
    }

    PrepareStatus WINAPI prepare_preview(
        const wchar_t* path,
        const wchar_t* language_tag,
        PreparedPreview* preview) noexcept
    {
        if (path == nullptr || preview == nullptr || preview->size < sizeof(PreparedPreview))
        {
            return PrepareStatus::failed;
        }

        try
        {
            const auto result = glance::app::prepare_office_pdf(path);
            switch (result.status)
            {
            case glance::app::OfficePdfStatus::success:
            {
                if (result.pdf_path.size() + 1 > preview_path_capacity)
                {
                    return PrepareStatus::failed;
                }
                PreparedPreview prepared;
                prepared.kind = PreviewContentKind::document;
                prepared.format = PreviewContentFormat::pdf;
                std::copy(result.pdf_path.begin(), result.pdf_path.end(), prepared.path);
                prepared.path[result.pdf_path.size()] = L'\0';
                *preview = prepared;
                return PrepareStatus::success;
            }
            case glance::app::OfficePdfStatus::unavailable:
                localize(
                    office_unavailable_key,
                    language_tag,
                    preview->error_detail);
                return PrepareStatus::unavailable;
            case glance::app::OfficePdfStatus::cancelled:
                return PrepareStatus::cancelled;
            default:
                localize(prepare_failed_key, language_tag, preview->error_detail);
                return PrepareStatus::failed;
            }
        }
        catch (...)
        {
            localize(prepare_failed_key, language_tag, preview->error_detail);
            return PrepareStatus::failed;
        }
    }

    void WINAPI release_preview(std::uint64_t) noexcept
    {
    }

    BOOL WINAPI query_interface(
        const GUID*,
        std::uint32_t,
        void** interface_pointer) noexcept
    {
        if (interface_pointer != nullptr)
        {
            *interface_pointer = nullptr;
        }
        return FALSE;
    }

    void WINAPI shutdown() noexcept
    {
        glance::app::shutdown_office_pdf_service();
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

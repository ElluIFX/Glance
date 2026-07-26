#include "pch.h"

#include "component_metadata.generated.h"
#include "glance/contracts/component_api.h"
#include "office_availability.h"
#include "office_pdf_service.h"
#include "../../version.h"

#include <algorithm>
#include <cwchar>

namespace
{
    using namespace glance::contracts::components;

    BOOL WINAPI query_health(const wchar_t* language_tag, HealthResult* result) noexcept
    {
        if (result == nullptr || result->size < sizeof(HealthResult))
        {
            return FALSE;
        }

        const auto mask = glance::app::office_available_components();
        result->severity = mask == glance::app::office_all_components
            ? HealthSeverity::healthy
            : HealthSeverity::warning;
        result->code = 0;
        result->capability_mask = mask;
        const bool chinese = language_tag != nullptr &&
            (_wcsnicmp(language_tag, L"zh", 2) == 0);
        const wchar_t* detail = nullptr;
        if (mask == glance::app::office_all_components)
        {
            detail = chinese ? L"Office COM 自动化可用" : L"Office COM automation available";
        }
        else if (mask == 0)
        {
            detail = chinese
                ? L"未检测到可用的 Office COM 自动化"
                : L"Office COM automation unavailable";
        }
        else
        {
            detail = chinese
                ? L"部分 Office COM 自动化不可用"
                : L"Some Office COM applications are unavailable";
        }
        wcscpy_s(result->detail, detail);
        return TRUE;
    }

    BOOL WINAPI can_preview(const wchar_t* path) noexcept
    {
        return path != nullptr && glance::app::office_preview_available(path);
    }

    PrepareStatus WINAPI prepare_preview(
        const wchar_t* path,
        wchar_t* output_path,
        std::uint32_t output_path_capacity) noexcept
    {
        if (path == nullptr || output_path == nullptr || output_path_capacity == 0)
        {
            return PrepareStatus::failed;
        }

        try
        {
            const auto result = glance::app::prepare_office_pdf(path);
            switch (result.status)
            {
            case glance::app::OfficePdfStatus::success:
                if (result.pdf_path.size() + 1 > output_path_capacity)
                {
                    return PrepareStatus::failed;
                }
                std::copy(result.pdf_path.begin(), result.pdf_path.end(), output_path);
                output_path[result.pdf_path.size()] = L'\0';
                return PrepareStatus::success;
            case glance::app::OfficePdfStatus::unavailable:
                return PrepareStatus::unavailable;
            case glance::app::OfficePdfStatus::cancelled:
                return PrepareStatus::cancelled;
            default:
                return PrepareStatus::failed;
            }
        }
        catch (...)
        {
            return PrepareStatus::failed;
        }
    }

    void WINAPI shutdown() noexcept
    {
        glance::app::shutdown_office_pdf_service();
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
    wcscpy_s(result.component_id, GLANCE_COMPONENT_ID_WSTRING);
    wcscpy_s(result.target_app_version, GLANCE_VERSION_WSTRING);
    result.output_kind = PreviewOutputKind::pdf_file;
    result.query_health = query_health;
    result.can_preview = can_preview;
    result.prepare_preview = prepare_preview;
    result.shutdown = shutdown;
    *api = result;
    return TRUE;
}

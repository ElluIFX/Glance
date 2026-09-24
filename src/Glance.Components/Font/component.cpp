#include "../Common/preview_cancellation.h"
#include "../../version.h"
#include "view.h"
#include <array>

namespace
{
using namespace glance::contracts::components;
constexpr std::array extensions{L".ttf", L".otf", L".ttc", L".otc", L".woff", L".woff2"};
BOOL WINAPI initialize(const ComponentRegistrar *registrar, ComponentRegistration *result) noexcept
{
    if (!registrar || !result || registrar->size < sizeof(*registrar) || result->size < sizeof(*result) ||
        !registrar->register_extension || !registrar->register_renderer)
        return FALSE;
    for (const auto extension : extensions)
        if (!registrar->register_extension(registrar->context, extension, GalleryMediaKind::none))
            return FALSE;
    if (!registrar->register_renderer(registrar->context, PreviewContentKind::document,
                                      PreviewContentFormat::component_view, &component_view_api_id,
                                      component_view_api_version))
        return FALSE;
    *result = ComponentRegistration{};
    wcscpy_s(result->component_id, L"font");
    wcscpy_s(result->target_app_version, GLANCE_VERSION_WSTRING);
    wcscpy_s(result->resource_path, L"resources.pri");
    result->preferred_kind = PreviewContentKind::document;
    result->preferred_format = PreviewContentFormat::component_view;
    return TRUE;
}
BOOL WINAPI status(ComponentStatusResult *result) noexcept
{
    if (!result || result->size < sizeof(*result))
        return FALSE;
    *result = ComponentStatusResult{};
    result->severity = HealthSeverity::healthy;
    wcscpy_s(result->display_name_key, L"ComponentName");
    wcscpy_s(result->detail_key, L"Available");
    return TRUE;
}
BOOL WINAPI loading(const wchar_t *, ComponentLoadingTextResult *result) noexcept
{
    if (!result || result->size < sizeof(*result))
        return FALSE;
    wcscpy_s(result->key, L"Loading");
    return TRUE;
}
BOOL WINAPI accepts(const wchar_t *path) noexcept
{
    try
    {
        if (!path)
            return FALSE;
        const auto extension = std::filesystem::path(path).extension().wstring();
        for (auto candidate : extensions)
            if (_wcsicmp(extension.c_str(), candidate) == 0)
                return TRUE;
    }
    catch (...)
    {
    }
    return FALSE;
}
PrepareStatus WINAPI prepare(const wchar_t *path, PreparedPreview *result) noexcept
{
    if (!result || result->size < sizeof(*result) || !accepts(path) || wcslen(path) >= preview_path_capacity)
        return PrepareStatus::unavailable;
    *result = PreparedPreview{};
    result->kind = PreviewContentKind::document;
    result->format = PreviewContentFormat::component_view;
    wcscpy_s(result->path, path);
    return PrepareStatus::success;
}
void WINAPI release(std::uint64_t) noexcept
{
}
void WINAPI shutdown() noexcept
{
}
BOOL WINAPI query(const GUID *id, std::uint32_t version, void **output) noexcept
{
    if (!output)
        return FALSE;
    *output = nullptr;
    if (id && IsEqualGUID(*id, component_view_api_id) && version == component_view_api_version)
    {
        *output = const_cast<ComponentViewApi *>(&glance::font::view_api());
        return TRUE;
    }
    return FALSE;
}
} // namespace
extern "C" __declspec(dllexport) BOOL WINAPI
GlanceComponentGetApi(std::uint32_t abi, glance::contracts::components::ComponentApi *result) noexcept
{
    using namespace glance::contracts::components;
    if (abi != abi_version || !result || result->size < sizeof(*result))
        return FALSE;
    *result = ComponentApi{.initialize = initialize,
                           .query_status = status,
                           .query_loading_text = loading,
                           .can_preview = accepts,
                           .prepare_preview = glance::components::prepare_preview_callback<prepare>,
                           .release_preview = release,
                           .query_interface = query,
                           .shutdown = shutdown};
    return TRUE;
}

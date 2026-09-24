#include "../Common/preview_cancellation.h"
#include "view.h"
#include <windows.h>
#include <array>
#include <filesystem>
#include <cwchar>
#include <memory>
#include "glance/contracts/component_api.h"
#include "../../version.h"

namespace
{
    using namespace glance::contracts::components;
    constexpr std::array extensions{L".exe", L".dll", L".sys", L".ocx", L".cpl",
                                    L".scr", L".pyd", L".efi", L".mui"};

    BOOL WINAPI initialize(const ComponentRegistrar* registrar, ComponentRegistration* result) noexcept
    {
        if (!registrar || !result || registrar->size < sizeof(*registrar) || result->size < sizeof(*result) ||
            !registrar->register_extension || !registrar->register_renderer)
            return FALSE;
        for (const auto extension : extensions)
            if (!registrar->register_extension(registrar->context, extension, GalleryMediaKind::none))
                return FALSE;
        if (!registrar->register_renderer(
                registrar->context, PreviewContentKind::document, PreviewContentFormat::component_view,
                &component_view_api_id, component_view_api_version))
            return FALSE;
        *result = ComponentRegistration{};
        wcscpy_s(result->component_id, L"executable");
        wcscpy_s(result->target_app_version, GLANCE_VERSION_WSTRING);
        wcscpy_s(result->resource_path, L"resources.pri");
        result->preferred_kind = PreviewContentKind::document;
        result->preferred_format = PreviewContentFormat::component_view;
        return TRUE;
    }
    BOOL WINAPI status(ComponentStatusResult* result) noexcept
    {
        if (!result || result->size < sizeof(*result))
            return FALSE;
        *result = ComponentStatusResult{};
        result->severity = HealthSeverity::healthy;
        wcscpy_s(result->display_name_key, L"Component.DisplayName");
        wcscpy_s(result->detail_key, L"Status.Available");
        return TRUE;
    }
    BOOL WINAPI loading(const wchar_t*, ComponentLoadingTextResult* result) noexcept
    {
        if (!result || result->size < sizeof(*result))
            return FALSE;
        wcscpy_s(result->key, L"Preview.Loading");
        return TRUE;
    }
    BOOL WINAPI accepts(const wchar_t* path) noexcept
    {
        try
        {
            if (!path)
                return FALSE;
            const auto extension = std::filesystem::path(path).extension().wstring();
            for (const auto candidate : extensions)
                if (_wcsicmp(extension.c_str(), candidate) == 0)
                    return TRUE;
        }
        catch (...)
        {
        }
        return FALSE;
    }
    PrepareStatus WINAPI prepare(const wchar_t* path, PreparedPreview* result) noexcept
    {
        if (!result || result->size < sizeof(*result) || !accepts(path) ||
            wcslen(path) >= preview_path_capacity)
            return PrepareStatus::unavailable;
        const auto handle =
            CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return PrepareStatus::failed;
        const auto close = [](void* file) { CloseHandle(file); };
        std::unique_ptr<void, decltype(close)> owner(handle, close);
        IMAGE_DOS_HEADER dos{};
        DWORD received{};
        if (!ReadFile(handle, &dos, sizeof(dos), &received, nullptr) || received != sizeof(dos) ||
            dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0)
            return PrepareStatus::unavailable;
        LARGE_INTEGER position{};
        position.QuadPart = dos.e_lfanew;
        DWORD signature{};
        if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) ||
            !ReadFile(handle, &signature, sizeof(signature), &received, nullptr) ||
            received != sizeof(signature) || signature != IMAGE_NT_SIGNATURE)
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
        glance::executable::shutdown_views();
    }
    BOOL WINAPI query(const GUID* id, std::uint32_t version, void** output) noexcept
    {
        if (!output)
            return FALSE;
        *output = nullptr;
        if (id && IsEqualGUID(*id, component_view_api_id) &&
            version == component_view_api_version)
        {
            *output = const_cast<ComponentViewApi*>(&glance::executable::view_api());
            return TRUE;
        }
        return FALSE;
    }
} // namespace
extern "C" __declspec(dllexport) BOOL WINAPI
GlanceComponentGetApi(std::uint32_t abi, glance::contracts::components::ComponentApi* result) noexcept
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

#include "pch.h"

#include "../Common/component_localization.h"
#include "../../version.h"
#include "glance/contracts/component_api.h"

#include <array>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace
{
    using namespace glance::contracts::components;

    constexpr std::array model_extensions{
        L".stl",
        L".3mf",
        L".obj",
        L".ply",
        L".gltf",
        L".glb",
        L".fbx" };
    constexpr wchar_t viewer_host[] = L"glance-model-viewer.invalid";
    constexpr wchar_t model_host[] = L"glance-model-source.invalid";
    constexpr wchar_t display_name_key[] = L"Component.DisplayName";
    constexpr wchar_t status_key[] = L"Status.Available";
    constexpr wchar_t loading_key[] = L"Preview.Loading";
    constexpr wchar_t failed_key[] = L"Preview.Failed";
    constexpr wchar_t viewer_loading_key[] = L"Viewer.Loading";
    constexpr wchar_t viewer_failed_key[] = L"Viewer.Failed";
    constexpr wchar_t viewer_empty_key[] = L"Viewer.Empty";
    constexpr wchar_t fit_key[] = L"Viewer.Fit";
    constexpr wchar_t grid_key[] = L"Viewer.Grid";
    constexpr wchar_t wireframe_key[] = L"Viewer.Wireframe";

    struct PreviewLease
    {
        std::filesystem::path source_path;
        std::wstring language_tag;
    };

    glance::components::ComponentResourceStore component_resources;
    std::mutex lease_mutex;
    std::unordered_map<std::uint64_t, PreviewLease> leases;
    std::atomic_uint64_t next_lease_token{ 1 };

    std::filesystem::path component_directory()
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
        const DWORD length =
            GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size())
        {
            return {};
        }
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

    template <std::size_t Size>
    bool localize(
        const wchar_t* key,
        const wchar_t* language_tag,
        wchar_t (&destination)[Size]) noexcept
    {
        return component_resources.copy(key, language_tag, destination, Size);
    }

    std::wstring localized_string(
        const wchar_t* key,
        const wchar_t* language_tag)
    {
        wchar_t text[loading_text_capacity]{};
        return localize(key, language_tag, text) ? std::wstring(text) : std::wstring{};
    }

    std::wstring lower_extension(const std::filesystem::path& path)
    {
        auto extension = path.extension().wstring();
        for (auto& character : extension)
        {
            character = static_cast<wchar_t>(std::towlower(character));
        }
        return extension;
    }

    bool supported_extension(std::wstring_view extension) noexcept
    {
        for (const auto* supported : model_extensions)
        {
            if (extension == supported)
            {
                return true;
            }
        }
        return false;
    }

    std::string utf8_from_wide(std::wstring_view value)
    {
        if (value.empty())
        {
            return {};
        }
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        {
            return {};
        }
        const int length = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (length <= 0)
        {
            return {};
        }
        std::string result(static_cast<std::size_t>(length), '\0');
        if (WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                value.data(),
                static_cast<int>(value.size()),
                result.data(),
                length,
                nullptr,
                nullptr) != length)
        {
            return {};
        }
        return result;
    }

    std::wstring url_encode(std::wstring_view value)
    {
        static constexpr wchar_t hexadecimal[] = L"0123456789ABCDEF";
        const auto utf8 = utf8_from_wide(value);
        std::wstring encoded;
        encoded.reserve(utf8.size() * 3);
        for (const unsigned char byte : utf8)
        {
            const bool unreserved =
                (byte >= 'a' && byte <= 'z') ||
                (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') ||
                byte == '-' ||
                byte == '_' ||
                byte == '.' ||
                byte == '~';
            if (unreserved)
            {
                encoded.push_back(static_cast<wchar_t>(byte));
            }
            else
            {
                encoded.push_back(L'%');
                encoded.push_back(hexadecimal[byte >> 4]);
                encoded.push_back(hexadecimal[byte & 0x0f]);
            }
        }
        return encoded;
    }

    void append_parameter(
        std::wstring& uri,
        std::wstring_view name,
        std::wstring_view value)
    {
        uri.push_back(uri.find(L'?') == std::wstring::npos ? L'?' : L'&');
        uri.append(name);
        uri.push_back(L'=');
        uri.append(url_encode(value));
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
        for (const auto* extension : model_extensions)
        {
            if (!registrar->register_extension(registrar->context, extension))
            {
                return FALSE;
            }
        }

        ComponentRegistration result;
        wcscpy_s(result.component_id, L"model3d");
        wcscpy_s(result.target_app_version, GLANCE_VERSION_WSTRING);
        result.preferred_kind = PreviewContentKind::web;
        result.preferred_format = PreviewContentFormat::html;
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
        if (path == nullptr)
        {
            return FALSE;
        }
        try
        {
            const std::filesystem::path source(path);
            std::error_code error;
            return supported_extension(lower_extension(source)) &&
                std::filesystem::is_regular_file(source, error);
        }
        catch (...)
        {
            return FALSE;
        }
    }

    PrepareStatus WINAPI prepare_preview(
        const wchar_t* path,
        const wchar_t* language_tag,
        PreparedPreview* preview) noexcept
    {
        if (path == nullptr ||
            preview == nullptr ||
            preview->size < sizeof(PreparedPreview) ||
            !can_preview(path))
        {
            return PrepareStatus::unavailable;
        }
        try
        {
            const auto viewer_path = component_directory() / L"web" / L"index.html";
            std::error_code error;
            if (!std::filesystem::is_regular_file(viewer_path, error))
            {
                localize(failed_key, language_tag, preview->error_detail);
                return PrepareStatus::failed;
            }

            std::uint64_t lease_token =
                next_lease_token.fetch_add(1, std::memory_order_relaxed);
            if (lease_token == 0)
            {
                lease_token =
                    next_lease_token.fetch_add(1, std::memory_order_relaxed);
            }
            {
                std::scoped_lock lock(lease_mutex);
                leases.insert_or_assign(
                    lease_token,
                    PreviewLease{
                        .source_path = std::filesystem::absolute(path),
                        .language_tag =
                            language_tag != nullptr ? language_tag : L"" });
            }

            const auto output_path = viewer_path.wstring();
            if (output_path.size() + 1 > preview_path_capacity)
            {
                std::scoped_lock lock(lease_mutex);
                leases.erase(lease_token);
                localize(failed_key, language_tag, preview->error_detail);
                return PrepareStatus::failed;
            }

            PreparedPreview prepared;
            prepared.kind = PreviewContentKind::web;
            prepared.format = PreviewContentFormat::html;
            prepared.lease_token = lease_token;
            wcscpy_s(prepared.path, output_path.c_str());
            *preview = prepared;
            return PrepareStatus::success;
        }
        catch (...)
        {
            localize(failed_key, language_tag, preview->error_detail);
            return PrepareStatus::failed;
        }
    }

    void WINAPI release_preview(std::uint64_t lease_token) noexcept
    {
        try
        {
            std::scoped_lock lock(lease_mutex);
            leases.erase(lease_token);
        }
        catch (...)
        {
        }
    }

    BOOL WINAPI query_web_preview(
        std::uint64_t lease_token,
        const WebPreviewOptions* options,
        WebPreviewDescriptor* descriptor) noexcept
    {
        if (options == nullptr ||
            options->size < sizeof(WebPreviewOptions) ||
            descriptor == nullptr ||
            descriptor->size < sizeof(WebPreviewDescriptor))
        {
            return FALSE;
        }
        try
        {
            PreviewLease lease;
            {
                std::scoped_lock lock(lease_mutex);
                const auto iterator = leases.find(lease_token);
                if (iterator == leases.end())
                {
                    return FALSE;
                }
                lease = iterator->second;
            }

            const auto viewer_root = component_directory() / L"web";
            const auto source_root = lease.source_path.parent_path();
            if (viewer_root.empty() || source_root.empty())
            {
                return FALSE;
            }

            const auto loading =
                localized_string(viewer_loading_key, lease.language_tag.c_str());
            const auto failed =
                localized_string(viewer_failed_key, lease.language_tag.c_str());
            const auto empty =
                localized_string(viewer_empty_key, lease.language_tag.c_str());
            const auto fit =
                localized_string(fit_key, lease.language_tag.c_str());
            const auto grid =
                localized_string(grid_key, lease.language_tag.c_str());
            const auto wireframe =
                localized_string(wireframe_key, lease.language_tag.c_str());
            if (loading.empty() ||
                failed.empty() ||
                empty.empty() ||
                fit.empty() ||
                grid.empty() ||
                wireframe.empty())
            {
                return FALSE;
            }

            const std::wstring model_uri =
                std::wstring(L"https://") + model_host + L"/" +
                url_encode(lease.source_path.filename().wstring());
            std::wstring navigation =
                std::wstring(L"https://") + viewer_host + L"/index.html";
            append_parameter(navigation, L"model", model_uri);
            append_parameter(
                navigation,
                L"extension",
                lower_extension(lease.source_path));
            append_parameter(
                navigation,
                L"theme",
                options->color_scheme == PreviewColorScheme::dark
                    ? L"dark"
                    : L"light");
            append_parameter(navigation, L"loading", loading);
            append_parameter(navigation, L"failed", failed);
            append_parameter(navigation, L"empty", empty);
            append_parameter(navigation, L"fit", fit);
            append_parameter(navigation, L"grid", grid);
            append_parameter(navigation, L"wireframe", wireframe);
            if (navigation.size() + 1 > preview_path_capacity)
            {
                return FALSE;
            }

            auto result = std::make_unique<WebPreviewDescriptor>();
            wcscpy_s(result->navigation_uri, navigation.c_str());
            result->mapping_count = 2;
            wcscpy_s(result->mappings[0].host_name, viewer_host);
            wcscpy_s(
                result->mappings[0].folder_path,
                viewer_root.wstring().c_str());
            result->mappings[0].access_kind = WebResourceAccessKind::deny_cors;
            wcscpy_s(result->mappings[1].host_name, model_host);
            wcscpy_s(
                result->mappings[1].folder_path,
                source_root.wstring().c_str());
            result->mappings[1].access_kind = WebResourceAccessKind::allow;
            *descriptor = *result;
            return TRUE;
        }
        catch (...)
        {
            return FALSE;
        }
    }

    WebPreviewApi web_preview_api{
        .query_preview = query_web_preview };

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
        if (interface_id == nullptr ||
            minimum_version > web_preview_api_version ||
            !IsEqualGUID(*interface_id, web_preview_api_id))
        {
            return FALSE;
        }
        *interface_pointer = &web_preview_api;
        return TRUE;
    }

    void WINAPI shutdown() noexcept
    {
        {
            std::scoped_lock lock(lease_mutex);
            leases.clear();
        }
        component_resources.shutdown();
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI GlanceComponentGetApi(
    std::uint32_t host_abi,
    glance::contracts::components::ComponentApi* api) noexcept
{
    using namespace glance::contracts::components;
    if (host_abi != abi_version ||
        api == nullptr ||
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

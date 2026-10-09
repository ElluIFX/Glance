#include "pch.h"
#include "glance/contracts/storage.h"
#include "component_loader.h"
#include "dependencies/dependency_service.h"

#include "localization.h"
#include "webview_availability.h"
#include "glance/contracts/diagnostics.h"
#include "../../version.h"


#include <algorithm>
#include <set>
#include <map>
#include <stdexcept>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace
{
    using glance::contracts::components::ComponentApi;
    using glance::contracts::components::ComponentLoadingTextResult;
    using glance::contracts::components::ComponentRegistration;
    using glance::contracts::components::GetApiFunction;
    using glance::contracts::components::GalleryMediaKind;
    using glance::contracts::components::HealthSeverity;
    using glance::contracts::components::InformationProviderApi;
    using glance::contracts::components::ImageMetadataApi;
    using glance::contracts::components::ImageMetadataEntry;
    using glance::contracts::components::ImageMetadataSink;
    using glance::contracts::components::FileDirectoryPreviewApi;
    using glance::contracts::components::PreparedPreview;
    using glance::contracts::components::RendererHostDescriptor;
    using glance::contracts::components::PreviewHostProtocol;
    using glance::contracts::components::HostRendererApi;
    using glance::contracts::components::PreviewContentFormat;
    using glance::contracts::components::PreviewContentKind;
    using glance::contracts::components::ProgressivePreviewApi;
    using glance::contracts::components::SettingsContributionApi;
    using glance::contracts::components::StatusBarShortcutApi;
    using glance::contracts::components::WebPreviewApi;
    using glance::contracts::components::WebPreviewDescriptor;
    using glance::contracts::components::WebPreviewOptions;
    using glance::contracts::components::WebResourceAccessKind;

    struct ComponentManifest
    {
        std::wstring id;
        std::wstring entry_point;
        std::vector<std::wstring> dependencies;
    };

    struct RendererRegistration
    {
        PreviewContentKind kind{ PreviewContentKind::none };
        PreviewContentFormat format{ PreviewContentFormat::none };
        GUID interface_id{};
        std::uint32_t interface_version{};
    };

    struct RegistrationCollector
    {
        std::vector<std::wstring> extensions;
        std::unordered_map<std::wstring, GalleryMediaKind> gallery_kinds;
        std::vector<RendererRegistration> renderers;
    };

    enum class DependencyFailure
    {
        none,
        missing,
        inactive,
        cycle,
    };

    struct LoadedComponent
    {
        std::wstring id;
        std::filesystem::path directory;
        HMODULE module{};
        ComponentApi api;
        ComponentRegistration registration;
        std::array<std::wstring, 3> metadata_keys;
        std::optional<ProgressivePreviewApi> progressive_preview;
        std::optional<WebPreviewApi> web_preview;
        std::optional<HostRendererApi> host_renderer;
        std::optional<std::filesystem::path> paged_document_host;
        std::optional<glance::contracts::components::ComponentViewApi> component_view;
        std::optional<std::filesystem::path> native_preview_host;
        std::optional<std::filesystem::path> native_media_host;
        std::optional<SettingsContributionApi> settings_contribution;
        std::optional<FileDirectoryPreviewApi> file_directory_preview;
        std::optional<ImageMetadataApi> image_metadata;
        std::optional<InformationProviderApi> information_provider;
        std::optional<StatusBarShortcutApi> status_bar_shortcut;
        std::vector<std::wstring> extensions;
        std::unordered_map<std::wstring, GalleryMediaKind> gallery_kinds;
        std::vector<std::wstring> dependencies;
        std::vector<RendererRegistration> renderers;
        std::wstring dependency_name;
        DependencyFailure dependency_failure{ DependencyFailure::none };
        bool activation_ready{ true };
        bool active{};
        bool resources_registered{};

        ~LoadedComponent()
        {
            if (resources_registered)
            {
                glance::app::unregister_component_resources(id);
            }
            if (api.shutdown != nullptr)
            {
                api.shutdown();
            }
            glance::app::dependencies::unregister_consumer(id);
            if (module != nullptr)
            {
                FreeLibrary(module);
            }
        }

        LoadedComponent() = default;
        LoadedComponent(const LoadedComponent&) = delete;
        LoadedComponent& operator=(const LoadedComponent&) = delete;
    };

    struct PreviewLease
    {
        std::shared_ptr<LoadedComponent> component;
        std::uint64_t token{};

        PreviewLease(
            std::shared_ptr<LoadedComponent> owner,
            std::uint64_t lease_token) noexcept
            : component(std::move(owner)), token(lease_token)
        {
        }

        PreviewLease(const PreviewLease&) = delete;
        PreviewLease& operator=(const PreviewLease&) = delete;

        ~PreviewLease()
        {
            if (component != nullptr && component->api.release_preview != nullptr)
            {
                component->api.release_preview(token);
            }
        }
    };

    struct RefinementSession
    {
        std::shared_ptr<LoadedComponent> component;
        std::shared_ptr<void> initial_lease;
        ProgressivePreviewApi api;
        std::shared_ptr<std::atomic_bool> cancellation;
        glance::contracts::components::PreviewPreparationOptions options;
        PreviewContentKind kind{ PreviewContentKind::none };
        PreviewContentFormat format{ PreviewContentFormat::none };
        std::uint64_t token{};
    };

    struct FileDirectorySession
    {
        std::shared_ptr<LoadedComponent> component;
        std::shared_ptr<void> lease;
        FileDirectoryPreviewApi api;
        std::uint64_t token{};
    };

    std::once_flag initialization_flag;
    std::mutex registry_mutex;
    std::vector<std::shared_ptr<LoadedComponent>> registered_components;
    std::unordered_map<
        std::wstring,
        std::vector<std::shared_ptr<LoadedComponent>>> extension_index;
    std::unordered_map<
        std::uint64_t,
        std::vector<std::shared_ptr<LoadedComponent>>> renderer_index;
    std::unordered_map<std::wstring, GalleryMediaKind> gallery_media_index;
    std::unordered_map<GalleryMediaKind, std::vector<std::wstring>> gallery_extension_index;

    std::filesystem::path executable_directory()
    {
        std::wstring path(32768, L'\0');
        const DWORD length =
            GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size())
        {
            return {};
        }
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

    template <std::size_t Size>
    std::optional<std::wstring> bounded_string(const wchar_t (&value)[Size])
    {
        const auto length = wcsnlen_s(value, Size);
        if (length == Size)
        {
            return std::nullopt;
        }
        return std::wstring(value, length);
    }

    bool valid_setting_id(std::wstring_view id) noexcept
    {
        return !id.empty() &&
            id.size() < glance::contracts::components::setting_id_capacity &&
            std::ranges::all_of(id, [](wchar_t character) {
                return (character >= L'a' && character <= L'z') ||
                    (character >= L'0' && character <= L'9') ||
                    character == L'-';
            });
    }

    bool valid_resource_key(std::wstring_view key) noexcept
    {
        return !key.empty() &&
            key.size() < glance::contracts::components::resource_key_capacity &&
            std::ranges::all_of(key, [](wchar_t character) {
                return (character >= L'a' && character <= L'z') ||
                    (character >= L'A' && character <= L'Z') ||
                    (character >= L'0' && character <= L'9') ||
                    character == L'.' || character == L'-' ||
                    character == L'_' || character == L'/';
            });
    }

    std::wstring localize_component_key(
        const LoadedComponent& component,
        std::wstring_view key)
    {
        return valid_resource_key(key)
            ? glance::app::localize_component(component.id, key)
            : std::wstring{};
    }

    void log_component_failure(
        const std::filesystem::path& directory,
        std::wstring_view reason) noexcept
    {
        glance::contracts::log_event(
            L"Skipping component '" + directory.wstring() + L"': " +
            std::wstring(reason));
    }

    bool valid_component_id(std::wstring_view id) noexcept
    {
        if (id.empty() || id.size() >= glance::contracts::components::component_id_capacity ||
            !std::iswlower(id.front()))
        {
            return false;
        }
        return std::ranges::all_of(id, [](wchar_t character) {
            return (character >= L'a' && character <= L'z') ||
                (character >= L'0' && character <= L'9') ||
                character == L'-';
        });
    }

    std::wstring normalize_extension(std::wstring_view extension)
    {
        if (extension.size() < 2 || extension.size() > 32 || extension.front() != L'.')
        {
            return {};
        }
        std::wstring normalized(extension);
        std::ranges::transform(normalized, normalized.begin(), [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
        if (!std::ranges::all_of(normalized.substr(1), [](wchar_t character) {
                return (character >= L'a' && character <= L'z') ||
                    (character >= L'0' && character <= L'9') ||
                    character == L'+' || character == L'-' || character == L'_';
            }))
        {
            return {};
        }
        return normalized;
    }

    bool valid_content_pair(
        PreviewContentKind kind,
        PreviewContentFormat format) noexcept
    {
        switch (kind)
        {
        case PreviewContentKind::text:
            return format == PreviewContentFormat::plain_text ||
                format == PreviewContentFormat::markdown;
        case PreviewContentKind::image:
            return format == PreviewContentFormat::image_file;
        case PreviewContentKind::media:
            return format == PreviewContentFormat::media_file;
        case PreviewContentKind::document:
            return format == PreviewContentFormat::pdf ||
                format == PreviewContentFormat::native_surface ||
                format == PreviewContentFormat::component_view;
        case PreviewContentKind::web:
            return format == PreviewContentFormat::html;
        case PreviewContentKind::directory:
            return format == PreviewContentFormat::file_directory;
        default:
            return false;
        }
    }

    bool valid_file_directory_value_kind(
        glance::contracts::components::FileDirectoryValueKind kind) noexcept
    {
        using glance::contracts::components::FileDirectoryValueKind;
        return kind == FileDirectoryValueKind::none ||
            kind == FileDirectoryValueKind::text ||
            kind == FileDirectoryValueKind::unsigned_integer ||
            kind == FileDirectoryValueKind::bytes ||
            kind == FileDirectoryValueKind::timestamp ||
            kind == FileDirectoryValueKind::ratio;
    }

    std::uint64_t renderer_key(
        PreviewContentKind kind,
        PreviewContentFormat format) noexcept
    {
        return static_cast<std::uint64_t>(kind) << 32U |
            static_cast<std::uint32_t>(format);
    }

    bool read_manifest(
        const std::filesystem::path& path,
        ComponentManifest& manifest) noexcept
    {
        try
        {
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                return false;
            }
            const std::string json{
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>() };
            const auto object =
                winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(json));
            if (object.GetNamedNumber(L"schema_version") != 3.0)
            {
                return false;
            }
            manifest.id = object.GetNamedString(L"id");
            manifest.entry_point = object.GetNamedString(L"entry_point");
            const auto dependencies = object.GetNamedArray(L"dependencies");
            std::unordered_set<std::wstring> unique_dependencies;
            for (std::uint32_t index = 0; index < dependencies.Size(); ++index)
            {
                auto dependency = std::wstring(dependencies.GetStringAt(index));
                if (!valid_component_id(dependency) ||
                    dependency == manifest.id ||
                    !unique_dependencies.insert(dependency).second)
                {
                    return false;
                }
                manifest.dependencies.push_back(std::move(dependency));
            }
            const std::filesystem::path entry(manifest.entry_point);
            return valid_component_id(manifest.id) &&
                !manifest.entry_point.empty() &&
                entry == entry.filename() &&
                _wcsicmp(entry.extension().c_str(), L".dll") == 0;
        }
        catch (...)
        {
            return false;
        }
    }

    BOOL WINAPI register_extension(void* context, const wchar_t* extension, GalleryMediaKind gallery_kind) noexcept
    {
        if (context == nullptr || extension == nullptr ||
            (gallery_kind != GalleryMediaKind::none && gallery_kind != GalleryMediaKind::image &&
             gallery_kind != GalleryMediaKind::video && gallery_kind != GalleryMediaKind::audio))
        {
            return FALSE;
        }
        try
        {
            auto& extensions = static_cast<RegistrationCollector*>(context)->extensions;
            auto normalized = normalize_extension(extension);
            if (normalized.empty() ||
                std::ranges::find(extensions, normalized) != extensions.end())
            {
                return FALSE;
            }
            static_cast<RegistrationCollector*>(context)->gallery_kinds.emplace(normalized, gallery_kind);
            extensions.push_back(std::move(normalized));
            return TRUE;
        }
        catch (...)
        {
            return FALSE;
        }
    }

    BOOL WINAPI register_renderer(
        void* context,
        PreviewContentKind kind,
        PreviewContentFormat format,
        const GUID* interface_id,
        std::uint32_t interface_version) noexcept
    {
        if (context == nullptr || interface_id == nullptr || interface_version == 0 ||
            !valid_content_pair(kind, format))
        {
            return FALSE;
        }
        try
        {
            auto& renderers = static_cast<RegistrationCollector*>(context)->renderers;
            if (std::ranges::any_of(renderers, [kind, format](const auto& renderer) {
                    return renderer.kind == kind && renderer.format == format;
                }))
            {
                return FALSE;
            }
            renderers.push_back(RendererRegistration{
                .kind = kind,
                .format = format,
                .interface_id = *interface_id,
                .interface_version = interface_version });
            return TRUE;
        }
        catch (...)
        {
            return FALSE;
        }
    }

    std::shared_ptr<LoadedComponent> load_component(
        const std::filesystem::path& directory,
        const ComponentManifest& manifest)
    {
        auto component = std::make_shared<LoadedComponent>();
        component->directory = directory;
        component->dependencies = manifest.dependencies;
        component->module = LoadLibraryExW(
            (directory / manifest.entry_point).c_str(),
            nullptr,
            LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (component->module == nullptr)
        {
            return {};
        }

        const auto get_api = reinterpret_cast<GetApiFunction>(
            GetProcAddress(
                component->module,
                glance::contracts::components::get_api_export));
        if (get_api == nullptr ||
            !get_api(glance::contracts::components::abi_version, &component->api) ||
            component->api.size < sizeof(ComponentApi) ||
            component->api.abi != glance::contracts::components::abi_version ||
            component->api.initialize == nullptr ||
            component->api.query_status == nullptr ||
            component->api.query_interface == nullptr ||
            component->api.shutdown == nullptr)
        {
            return {};
        }

        RegistrationCollector collector;
        component->id = manifest.id;
        glance::contracts::components::ComponentRegistrar registrar{
            .context = &collector,
            .register_extension = register_extension,
            .register_renderer = register_renderer,
            .query_host_interface = [](const GUID* id, std::uint32_t version, void** result) noexcept -> BOOL {
                if (!result) return FALSE;
                *result = nullptr;
                if (id && IsEqualGUID(*id, glance::contracts::dependencies::host_api_id) &&
                    version <= glance::contracts::dependencies::host_api_version)
                {
                    *result = const_cast<glance::contracts::dependencies::HostApi*>(&glance::app::dependencies::host_api());
                    return TRUE;
                }
                return FALSE;
            } };
        if (!component->api.initialize(&registrar, &component->registration) ||
            component->registration.size < sizeof(ComponentRegistration))
        {
            return {};
        }
        const auto registered_id = bounded_string(component->registration.component_id);
        const auto target_version =
            bounded_string(component->registration.target_app_version);
        const auto resource_path = bounded_string(component->registration.resource_path);
        if (!registered_id.has_value() || manifest.id != *registered_id ||
            !target_version.has_value() || *target_version != GLANCE_VERSION_WSTRING ||
            !resource_path.has_value() || resource_path->empty())
        {
            return {};
        }
        const std::filesystem::path relative_resources(*resource_path);
        if (relative_resources.is_absolute() || relative_resources.has_root_path() ||
            std::ranges::any_of(relative_resources, [](const auto& part) {
                return part == L"..";
            }))
        {
            return {};
        }
        const auto absolute_resources = directory / relative_resources;
        std::error_code resource_error;
        if (!std::filesystem::is_regular_file(absolute_resources, resource_error))
        {
            return {};
        }
        component->id = manifest.id;
        if (!glance::app::register_component_resources(
                component->id,
                std::filesystem::absolute(absolute_resources)))
        {
            return {};
        }
        component->resources_registered = true;
        component->extensions = std::move(collector.extensions);
        component->gallery_kinds = std::move(collector.gallery_kinds);
        component->renderers = std::move(collector.renderers);
        void* interface_pointer{};
        using glance::contracts::components::ComponentMetadataApi;
        if (!component->api.query_interface(&glance::contracts::components::component_metadata_api_id,
            glance::contracts::components::component_metadata_api_version, &interface_pointer) || !interface_pointer)
        {
            glance::contracts::log_event(L"Component metadata interface is missing: " + component->id);
            return {};
        }
        const auto* metadata = static_cast<const ComponentMetadataApi*>(interface_pointer);
        if (metadata->size < sizeof(ComponentMetadataApi) || metadata->version != glance::contracts::components::component_metadata_api_version)
        {
            glance::contracts::log_event(L"Component metadata interface is invalid: " + component->id);
            return {};
        }
        const auto keys = std::array{bounded_string(metadata->summary_key), bounded_string(metadata->capabilities_key),
            bounded_string(metadata->dependencies_key)};
        for (std::size_t index = 0; index < keys.size(); ++index)
        {
            if (!keys[index] || !valid_resource_key(*keys[index]))
            {
                glance::contracts::log_event(L"Component metadata resource key is invalid: " + component->id);
                return {};
            }
            for (const auto* language : {L"en-US", L"zh-CN"})
                if (!glance::app::has_component_translation(component->id, *keys[index], language))
                {
                    glance::contracts::log_event(L"Component metadata translation is missing: " + component->id + L" / " + *keys[index] + L" / " + language);
                    return {};
                }
            component->metadata_keys[index] = *keys[index];
        }
        interface_pointer = nullptr;
        if (component->api.query_interface(
                &glance::contracts::components::progressive_preview_api_id,
                glance::contracts::components::progressive_preview_api_version,
                &interface_pointer) &&
            interface_pointer != nullptr)
        {
            const auto* interface_api =
                static_cast<const ProgressivePreviewApi*>(interface_pointer);
            if (interface_api->size >= sizeof(ProgressivePreviewApi) &&
                interface_api->version ==
                    glance::contracts::components::progressive_preview_api_version &&
                interface_api->can_refine != nullptr &&
                interface_api->query_refinement_text != nullptr &&
                interface_api->prepare_refined_preview != nullptr)
            {
                component->progressive_preview = *interface_api;
            }
        }
        interface_pointer = nullptr;
        if (component->api.query_interface(
                &glance::contracts::components::web_preview_api_id,
                glance::contracts::components::web_preview_api_version,
                &interface_pointer) &&
            interface_pointer != nullptr)
        {
            const auto* interface_api =
                static_cast<const WebPreviewApi*>(interface_pointer);
            if (interface_api->size >= sizeof(WebPreviewApi) &&
                interface_api->version ==
                    glance::contracts::components::web_preview_api_version &&
                interface_api->query_preview != nullptr)
            {
                component->web_preview = *interface_api;
            }
        }
        interface_pointer = nullptr;
        if (component->api.query_interface(
                &glance::contracts::components::host_renderer_api_id,
                glance::contracts::components::host_renderer_api_version,
                &interface_pointer) &&
            interface_pointer != nullptr)
        {
            const auto* interface_api =
                static_cast<const HostRendererApi*>(interface_pointer);
            if (interface_api->size >= sizeof(HostRendererApi) &&
                interface_api->version ==
                    glance::contracts::components::host_renderer_api_version &&
                interface_api->query_host != nullptr)
            {
                component->host_renderer = *interface_api;
            }
        }
        interface_pointer = nullptr;
        if (component->api.query_interface(
                &glance::contracts::components::component_view_api_id,
                glance::contracts::components::component_view_api_version, &interface_pointer) && interface_pointer)
        {
            const auto* api = static_cast<const glance::contracts::components::ComponentViewApi*>(interface_pointer);
            if (api->size >= sizeof(*api) && api->version == glance::contracts::components::component_view_api_version &&
                api->create && api->close && api->set_language)
                component->component_view = *api;
        }
        interface_pointer = nullptr;
        if (component->api.query_interface(
                &glance::contracts::components::settings_contribution_api_id,
                glance::contracts::components::settings_contribution_api_version,
                &interface_pointer) &&
            interface_pointer != nullptr)
        {
            const auto* interface_api =
                static_cast<const SettingsContributionApi*>(interface_pointer);
            if (interface_api->size >= sizeof(SettingsContributionApi) &&
                interface_api->version ==
                    glance::contracts::components::settings_contribution_api_version &&
                interface_api->register_settings != nullptr)
            {
                component->settings_contribution = *interface_api;
            }
        }
        interface_pointer = nullptr;
        if (component->api.query_interface(
                &glance::contracts::components::file_directory_preview_api_id,
                glance::contracts::components::file_directory_preview_api_version,
                &interface_pointer) &&
            interface_pointer != nullptr)
        {
            const auto* interface_api =
                static_cast<const FileDirectoryPreviewApi*>(interface_pointer);
            if (interface_api->size >= sizeof(FileDirectoryPreviewApi) &&
                interface_api->version ==
                    glance::contracts::components::file_directory_preview_api_version &&
                interface_api->open != nullptr &&
                interface_api->enumerate_children != nullptr)
            {
                component->file_directory_preview = *interface_api;
            }
        }
        interface_pointer = nullptr;
        if (component->api.query_interface(
                &glance::contracts::components::image_metadata_api_id,
                glance::contracts::components::image_metadata_api_version,
                &interface_pointer) &&
            interface_pointer != nullptr)
        {
            const auto* interface_api =
                static_cast<const ImageMetadataApi*>(interface_pointer);
            if (interface_api->size >= sizeof(ImageMetadataApi) &&
                interface_api->version ==
                    glance::contracts::components::image_metadata_api_version &&
                interface_api->query_metadata != nullptr)
            {
                component->image_metadata = *interface_api;
            }
        }
        interface_pointer = nullptr;
        if (component->api.query_interface(
                &glance::contracts::components::information_provider_api_id,
                glance::contracts::components::information_provider_api_version,
                &interface_pointer) &&
            interface_pointer != nullptr)
        {
            const auto* interface_api =
                static_cast<const InformationProviderApi*>(interface_pointer);
            if (interface_api->size >= sizeof(InformationProviderApi) &&
                interface_api->version ==
                    glance::contracts::components::information_provider_api_version &&
                (interface_api->query_info != nullptr || interface_api->query_json != nullptr ||
                    interface_api->identify_format != nullptr))
            {
                component->information_provider = *interface_api;
            }
        }
        interface_pointer = nullptr;
        if (component->api.query_interface(
                &glance::contracts::components::status_bar_shortcut_api_id,
                glance::contracts::components::status_bar_shortcut_api_version,
                &interface_pointer) &&
            interface_pointer != nullptr)
        {
            const auto* interface_api =
                static_cast<const StatusBarShortcutApi*>(interface_pointer);
            if (interface_api->size >= sizeof(StatusBarShortcutApi) &&
                interface_api->version ==
                    glance::contracts::components::status_bar_shortcut_api_version &&
                interface_api->enumerate_shortcuts != nullptr &&
                interface_api->query_state != nullptr &&
                interface_api->activate != nullptr)
            {
                component->status_bar_shortcut = *interface_api;
            }
        }
        const bool primary_preview = !component->extensions.empty();
        if (primary_preview)
        {
            if (component->api.can_preview == nullptr ||
                component->api.prepare_preview == nullptr ||
                component->api.release_preview == nullptr ||
                !valid_content_pair(
                    component->registration.preferred_kind,
                    component->registration.preferred_format))
            {
                return {};
            }
        }
        else if (component->registration.preferred_kind != PreviewContentKind::none ||
            component->registration.preferred_format != PreviewContentFormat::none ||
            (component->renderers.empty() &&
             !component->settings_contribution.has_value() &&
             !(component->information_provider && component->information_provider->identify_format) &&
             !component->status_bar_shortcut.has_value()))
        {
            return {};
        }
        if (std::ranges::any_of(component->renderers, [&component](const auto& renderer) {
                if (IsEqualGUID(renderer.interface_id, glance::contracts::components::component_view_api_id))
                    return !component->component_view || renderer.interface_version !=
                        glance::contracts::components::component_view_api_version;
                if (IsEqualGUID(
                        renderer.interface_id,
                        glance::contracts::components::host_renderer_api_id))
                {
                    return !component->host_renderer.has_value() ||
                        renderer.interface_version !=
                            glance::contracts::components::host_renderer_api_version;
                }
                if (IsEqualGUID(
                        renderer.interface_id,
                        glance::contracts::components::file_directory_preview_api_id))
                {
                    return !component->file_directory_preview.has_value() ||
                        renderer.interface_version !=
                            glance::contracts::components::file_directory_preview_api_version;
                }
                return true;
            }))
        {
            return {};
        }
        for (const auto& renderer : component->renderers)
        {
            if (!IsEqualGUID(renderer.interface_id, glance::contracts::components::host_renderer_api_id))
                continue;
            PreviewHostProtocol protocol;
            std::optional<std::filesystem::path>* target = nullptr;
            if (renderer.kind == PreviewContentKind::document && renderer.format == PreviewContentFormat::pdf)
            {
                protocol = PreviewHostProtocol::paged_document;
                target = &component->paged_document_host;
            }
            else if (renderer.kind == PreviewContentKind::document && renderer.format == PreviewContentFormat::native_surface)
            {
                protocol = PreviewHostProtocol::native_document;
                target = &component->native_preview_host;
            }
            else if (renderer.kind == PreviewContentKind::media && renderer.format == PreviewContentFormat::media_file)
            {
                protocol = PreviewHostProtocol::native_media;
                target = &component->native_media_host;
            }
            else
                return {};
            RendererHostDescriptor descriptor;
            const bool described = component->host_renderer->query_host(protocol, &descriptor) != FALSE &&
                descriptor.size >= sizeof(descriptor);
            const auto executable = described ? bounded_string(descriptor.host_executable) : std::nullopt;
            const std::filesystem::path relative = executable.has_value()
                ? std::filesystem::path(*executable) : std::filesystem::path{};
            std::error_code error;
            const bool available = executable.has_value() && !relative.empty() &&
                relative == relative.filename() &&
                _wcsicmp(relative.extension().c_str(), L".exe") == 0 &&
                std::filesystem::is_regular_file(directory / relative, error);
            component->activation_ready = component->activation_ready && available;
            if (available)
                *target = directory / relative;
        }
        std::ranges::sort(component->extensions);
        return component;
    }

    enum class DependencyVisit
    {
        none,
        visiting,
        complete,
    };

    bool activate_component(
        const std::shared_ptr<LoadedComponent>& component,
        const std::unordered_map<std::wstring, std::shared_ptr<LoadedComponent>>& components,
        std::unordered_map<std::wstring, DependencyVisit>& visits)
    {
        auto& visit = visits[component->id];
        if (visit == DependencyVisit::complete)
        {
            return component->active;
        }
        if (visit == DependencyVisit::visiting)
        {
            component->dependency_failure = DependencyFailure::cycle;
            return false;
        }

        visit = DependencyVisit::visiting;
        if (!component->activation_ready)
        {
            visit = DependencyVisit::complete;
            return false;
        }
        for (const auto& dependency_id : component->dependencies)
        {
            const auto dependency = components.find(dependency_id);
            if (dependency == components.end())
            {
                component->dependency_failure = DependencyFailure::missing;
                component->dependency_name = dependency_id;
                visit = DependencyVisit::complete;
                return false;
            }
            if (!activate_component(dependency->second, components, visits))
            {
                component->dependency_failure =
                    dependency->second->dependency_failure == DependencyFailure::cycle
                    ? DependencyFailure::cycle
                    : DependencyFailure::inactive;
                component->dependency_name = dependency_id;
                visit = DependencyVisit::complete;
                return false;
            }
        }
        component->active = true;
        visit = DependencyVisit::complete;
        return true;
    }

    void initialize_registry()
    {
        const auto root = executable_directory() / L"components";
        std::error_code error;
        if (!std::filesystem::is_directory(root, error))
        {
            return;
        }

        std::vector<std::filesystem::path> directories;
        for (std::filesystem::directory_iterator iterator(root, error), end;
             !error && iterator != end;
             iterator.increment(error))
        {
            if (iterator->is_directory(error))
            {
                directories.push_back(iterator->path());
            }
        }
        std::ranges::sort(directories, [](const auto& left, const auto& right) {
            return _wcsicmp(
                       left.filename().c_str(),
                       right.filename().c_str()) < 0;
        });

        std::vector<std::shared_ptr<LoadedComponent>> loaded;
        std::unordered_set<std::wstring> component_ids;
        for (const auto& directory : directories)
        {
            ComponentManifest manifest;
            if (!read_manifest(directory / L"component.json", manifest))
            {
                log_component_failure(directory, L"invalid manifest");
                continue;
            }
            if (component_ids.contains(manifest.id))
            {
                log_component_failure(directory, L"duplicate component id");
                continue;
            }
            auto component = load_component(directory, manifest);
            if (component == nullptr)
            {
                log_component_failure(directory, L"load or registration failed");
                continue;
            }
            component_ids.insert(component->id);
            loaded.push_back(std::move(component));
        }
        std::ranges::sort(loaded, [](const auto& left, const auto& right) {
            return left->id < right->id;
        });

        std::unordered_map<std::wstring, std::shared_ptr<LoadedComponent>> components_by_id;
        for (const auto& component : loaded)
        {
            components_by_id.emplace(component->id, component);
        }
        std::unordered_map<std::wstring, DependencyVisit> dependency_visits;
        for (const auto& component : loaded)
        {
            static_cast<void>(activate_component(
                component,
                components_by_id,
                dependency_visits));
        }

        std::unordered_map<
            std::wstring,
            std::vector<std::shared_ptr<LoadedComponent>>> index;
        std::unordered_map<
            std::uint64_t,
            std::vector<std::shared_ptr<LoadedComponent>>> renderers;
        std::unordered_map<std::wstring, GalleryMediaKind> gallery_media;
        std::unordered_set<std::wstring> conflicting_gallery_extensions;
        for (const auto& component : loaded)
        {
            if (!component->active)
            {
                continue;
            }
            for (const auto& extension : component->extensions)
            {
                index[extension].push_back(component);
                const auto kind = component->gallery_kinds.at(extension);
                if (kind != GalleryMediaKind::image &&
                    kind != GalleryMediaKind::video &&
                    kind != GalleryMediaKind::audio)
                {
                    continue;
                }
                const auto [match, inserted] = gallery_media.emplace(extension, kind);
                if (!inserted && match->second != kind)
                {
                    conflicting_gallery_extensions.insert(extension);
                }
            }
            for (const auto& renderer : component->renderers)
            {
                renderers[renderer_key(renderer.kind, renderer.format)].push_back(component);
            }
        }
        std::unordered_map<GalleryMediaKind, std::vector<std::wstring>> gallery_extensions;
        for (const auto& extension : conflicting_gallery_extensions)
        {
            gallery_media.erase(extension);
            glance::contracts::log_event(
                L"Ignoring conflicting component gallery classification for " + extension + L".");
        }
        for (const auto& [extension, kind] : gallery_media)
        {
            gallery_extensions[kind].push_back(extension);
        }
        for (auto& [kind, extensions] : gallery_extensions)
        {
            std::ranges::sort(extensions);
        }

        std::scoped_lock lock(registry_mutex);
        registered_components = std::move(loaded);
        extension_index = std::move(index);
        renderer_index = std::move(renderers);
        gallery_media_index = std::move(gallery_media);
        gallery_extension_index = std::move(gallery_extensions);
    }

    std::vector<std::shared_ptr<LoadedComponent>> candidates_for_path(
        std::wstring_view path)
    {
        const auto extension =
            normalize_extension(std::filesystem::path(path).extension().wstring());
        if (extension.empty())
        {
            return {};
        }
        std::scoped_lock lock(registry_mutex);
        const auto match = extension_index.find(extension);
        return match == extension_index.end() ? std::vector<std::shared_ptr<LoadedComponent>>{}
                                              : match->second;
    }

    std::wstring query_loading_text(
        const LoadedComponent& component,
        const std::wstring& path)
    {
        if (component.api.query_loading_text == nullptr)
        {
            return {};
        }

        ComponentLoadingTextResult result;
        if (!component.api.query_loading_text(
                path.c_str(),
                &result))
        {
            return {};
        }
        const auto key = bounded_string(result.key);
        return key.has_value()
            ? localize_component_key(component, *key)
            : std::wstring{};
    }

    std::wstring query_refinement_text(
        const RefinementSession& session)
    {
        ComponentLoadingTextResult result;
        if (!session.api.query_refinement_text(
                session.token,
                &result))
        {
            return {};
        }
        const auto key = bounded_string(result.key);
        return key.has_value()
            ? localize_component_key(*session.component, *key)
            : std::wstring{};
    }

    glance::app::ComponentPreviewResult materialize_preview(
        const std::shared_ptr<LoadedComponent>& component,
        const PreparedPreview& preview)
    {
        glance::app::ComponentPreviewResult result;
        result.status = glance::contracts::components::PrepareStatus::success;
        result.exclude_right_click_navigation = component->registration.exclude_right_click_navigation != FALSE;
        if (!valid_content_pair(preview.kind, preview.format))
        {
            component->api.release_preview(preview.lease_token);
            result.status = glance::contracts::components::PrepareStatus::failed;
            return result;
        }

        if (preview.kind == PreviewContentKind::directory &&
            preview.format == PreviewContentFormat::file_directory)
        {
            if (preview.lease_token == 0 ||
                !component->file_directory_preview.has_value())
            {
                component->api.release_preview(preview.lease_token);
                result.status = glance::contracts::components::PrepareStatus::failed;
                return result;
            }
            auto lease = std::make_shared<PreviewLease>(component, preview.lease_token);
            auto session = std::make_shared<FileDirectorySession>();
            session->component = component;
            session->lease = lease;
            session->api = *component->file_directory_preview;
            session->token = preview.lease_token;
            result.kind = preview.kind;
            result.format = preview.format;
            result.lease = lease;
            result.file_directory = std::move(session);
            return result;
        }

        if (preview.path[0] == L'\0')
        {
            component->api.release_preview(preview.lease_token);
            result.status = glance::contracts::components::PrepareStatus::failed;
            return result;
        }

        const std::filesystem::path output(preview.path);
        std::error_code error;
        if (!output.is_absolute() || !std::filesystem::is_regular_file(output, error))
        {
            component->api.release_preview(preview.lease_token);
            result.status = glance::contracts::components::PrepareStatus::failed;
            return result;
        }

        result.kind = preview.kind;
        result.component_id = component->id;
        result.format = preview.format;
        result.output_path = output.wstring();
        if (preview.format == PreviewContentFormat::component_view)
        {
            if (!component->component_view)
            {
                component->api.release_preview(preview.lease_token);
                result.status = glance::contracts::components::PrepareStatus::failed;
                return result;
            }
            result.component_view = std::make_shared<glance::app::ComponentViewRegistration>(
                glance::app::ComponentViewRegistration{*component->component_view, component, preview.lease_token});
        }
        result.lease = std::make_shared<PreviewLease>(
            component,
            preview.lease_token);
        if (preview.kind == PreviewContentKind::document &&
            preview.format == PreviewContentFormat::native_surface)
        {
            if (!component->native_preview_host.has_value())
            {
                result.lease.reset();
                result.status = glance::contracts::components::PrepareStatus::failed;
                return result;
            }
            result.native_renderer =
                std::make_shared<glance::app::NativePreviewRendererRegistration>(
                    glance::app::NativePreviewRendererRegistration{
                        .host_path = component->native_preview_host->wstring(),
                        .lease = std::static_pointer_cast<void>(component) });
        }
        if (preview.kind == PreviewContentKind::media &&
            preview.format == PreviewContentFormat::media_file &&
            component->native_media_host.has_value())
        {
            if (!component->native_media_host.has_value())
            {
                result.lease.reset();
                result.status = glance::contracts::components::PrepareStatus::failed;
                return result;
            }
            result.native_media_renderer =
                std::make_shared<glance::app::NativeMediaRendererRegistration>(
                    glance::app::NativeMediaRendererRegistration{
                        .component_id = component->id,
                        .host_path = component->native_media_host->wstring(),
                        .lease = std::static_pointer_cast<void>(component) });
        }
        return result;
    }

    bool valid_web_host(std::wstring_view host) noexcept
    {
        if (host.empty() ||
            host.size() >= glance::contracts::components::web_resource_host_capacity ||
            host.front() == L'.' ||
            host.back() == L'.')
        {
            return false;
        }
        return std::ranges::all_of(host, [](wchar_t character) {
            return (character >= L'a' && character <= L'z') ||
                (character >= L'0' && character <= L'9') ||
                character == L'-' ||
                character == L'.';
        });
    }

    std::shared_ptr<glance::app::ComponentWebPreview> materialize_web_preview(
        const LoadedComponent& component,
        std::uint64_t lease_token,
        glance::contracts::components::PreviewColorScheme color_scheme)
    {
        if (!component.web_preview.has_value() || lease_token == 0)
        {
            return {};
        }

        WebPreviewOptions options{ .color_scheme = color_scheme };
        auto descriptor = std::make_unique<WebPreviewDescriptor>();
        if (!component.web_preview->query_preview(
                lease_token,
                &options,
                descriptor.get()) ||
            descriptor->size < sizeof(WebPreviewDescriptor) ||
            descriptor->mapping_count == 0 ||
            descriptor->mapping_count >
                glance::contracts::components::maximum_web_resource_mappings ||
            descriptor->localized_parameter_count >
                glance::contracts::components::maximum_web_localized_parameters)
        {
            return {};
        }

        const auto navigation_length = wcsnlen_s(
            descriptor->navigation_uri,
            std::size(descriptor->navigation_uri));
        if (navigation_length == std::size(descriptor->navigation_uri))
        {
            return {};
        }

        auto result = std::make_shared<glance::app::ComponentWebPreview>();
        result->navigation_uri.assign(
            descriptor->navigation_uri,
            navigation_length);
        std::unordered_set<std::wstring> parameter_names;
        for (std::uint32_t index = 0;
             index < descriptor->localized_parameter_count;
             ++index)
        {
            const auto name = bounded_string(
                descriptor->localized_parameters[index].name);
            const auto key = bounded_string(
                descriptor->localized_parameters[index].resource_key);
            if (!name.has_value() || !valid_setting_id(*name) ||
                !parameter_names.insert(*name).second ||
                !key.has_value() || !valid_resource_key(*key))
            {
                return {};
            }
            const auto localized = localize_component_key(component, *key);
            result->navigation_uri.append(
                result->navigation_uri.find(L'?') != std::wstring::npos
                    ? L"&"
                    : L"?");
            result->navigation_uri.append(*name);
            result->navigation_uri.push_back(L'=');
            const auto encoded =
                winrt::Windows::Foundation::Uri::EscapeComponent(localized);
            result->navigation_uri.append(encoded.c_str());
            if (result->navigation_uri.size() >=
                glance::contracts::components::preview_path_capacity)
            {
                return {};
            }
        }
        std::unordered_set<std::wstring> hosts;
        for (std::uint32_t index = 0; index < descriptor->mapping_count; ++index)
        {
            const auto& mapping = descriptor->mappings[index];
            const auto host_length =
                wcsnlen_s(mapping.host_name, std::size(mapping.host_name));
            const auto folder_length =
                wcsnlen_s(mapping.folder_path, std::size(mapping.folder_path));
            if (host_length == std::size(mapping.host_name) ||
                folder_length == std::size(mapping.folder_path))
            {
                return {};
            }
            std::wstring host(mapping.host_name, host_length);
            std::wstring folder(mapping.folder_path, folder_length);
            std::error_code error;
            if (!valid_web_host(host) ||
                !hosts.insert(host).second ||
                !std::filesystem::path(folder).is_absolute() ||
                !std::filesystem::is_directory(folder, error) ||
                (mapping.access_kind != WebResourceAccessKind::deny_cors &&
                 mapping.access_kind != WebResourceAccessKind::allow))
            {
                return {};
            }
            result->mappings.push_back(glance::app::ComponentWebResourceMapping{
                .host_name = std::move(host),
                .folder_path = std::move(folder),
                .access_kind = mapping.access_kind });
        }

        const winrt::Windows::Foundation::Uri navigation(result->navigation_uri);
        if (_wcsicmp(navigation.SchemeName().c_str(), L"https") != 0 ||
            !hosts.contains(std::wstring(navigation.Host())))
        {
            return {};
        }
        return result;
    }

    struct FileDirectoryEntryCollector
    {
        const LoadedComponent* component{};
        std::vector<glance::app::FileDirectoryEntry>* entries{};
        bool valid{ true };
    };

    BOOL WINAPI append_file_directory_entry(
        void* context,
        const glance::contracts::components::FileDirectoryEntry* entry) noexcept
    {
        if (context == nullptr || entry == nullptr || entry->name == nullptr ||
            entry->node_id == 0 ||
            entry->value_count > glance::contracts::components::maximum_file_directory_columns ||
            (entry->value_count != 0 && entry->values == nullptr))
        {
            return FALSE;
        }
        auto& collector = *static_cast<FileDirectoryEntryCollector*>(context);
        try
        {
            glance::app::FileDirectoryEntry copied{
                .node_id = entry->node_id,
                .is_folder = entry->is_folder != FALSE,
                .has_children = entry->has_children != FALSE,
                .name = entry->name,
                .icon_key = entry->icon_key == nullptr ? L"" : entry->icon_key };
            copied.values.reserve(entry->value_count);
            for (std::uint32_t index = 0; index < entry->value_count; ++index)
            {
                const auto& value = entry->values[index];
                if (!valid_file_directory_value_kind(value.kind))
                {
                    collector.valid = false;
                    return FALSE;
                }
                std::wstring text = value.text == nullptr ? L"" : value.text;
                if (value.text_kind ==
                    glance::contracts::components::ComponentTextKind::resource_key)
                {
                    text = localize_component_key(*collector.component, text);
                }
                else if (value.text_kind !=
                         glance::contracts::components::ComponentTextKind::literal)
                {
                    collector.valid = false;
                    return FALSE;
                }
                copied.values.push_back(glance::app::FileDirectoryValue{
                    .kind = value.kind,
                    .unsigned_value = value.unsigned_value,
                    .ratio_value = value.ratio_value,
                    .text = std::move(text) });
            }
            collector.entries->push_back(std::move(copied));
            return TRUE;
        }
        catch (...)
        {
            collector.valid = false;
            return FALSE;
        }
    }


    struct HoverInfoCollector
    {
        std::wstring text;
        const std::atomic_bool* cancelled{};
        bool valid{ true };
    };

    BOOL WINAPI append_hover_info(
        void* context,
        const wchar_t* text,
        std::uint32_t length) noexcept
    {
        if (context == nullptr || (length != 0 && text == nullptr))
        {
            return FALSE;
        }
        auto& collector = *static_cast<HoverInfoCollector*>(context);
        if (collector.cancelled->load(std::memory_order_acquire) ||
            length > 256 * 1024 ||
            collector.text.size() > 256 * 1024 - length)
        {
            collector.valid = false;
            return FALSE;
        }
        if (length == 0)
        {
            return TRUE;
        }
        try
        {
            collector.text.append(text, length);
            return TRUE;
        }
        catch (...)
        {
            collector.valid = false;
            return FALSE;
        }
    }

    BOOL WINAPI hover_info_cancelled(void* context) noexcept
    {
        return context != nullptr &&
            static_cast<HoverInfoCollector*>(context)->cancelled->load(
                std::memory_order_acquire);
    }

    struct InformationPanelCollector
    {
        const LoadedComponent* component{};
        std::wstring text;
        const std::atomic_bool* cancelled{};
        bool valid{ true };
    };

    std::optional<std::wstring> resolve_information_panel_text(
        const InformationPanelCollector& collector,
        const glance::contracts::components::InformationPanelText& text)
    {
        const auto value = bounded_string(text.value);
        if (!value.has_value() ||
            text.argument_count >
                glance::contracts::components::maximum_information_panel_arguments)
        {
            return std::nullopt;
        }
        std::wstring result;
        if (text.kind == glance::contracts::components::ComponentTextKind::resource_key)
        {
            if (!valid_resource_key(*value))
            {
                return std::nullopt;
            }
            result = localize_component_key(*collector.component, *value);
        }
        else if (text.kind == glance::contracts::components::ComponentTextKind::literal)
        {
            result = *value;
        }
        else
        {
            return std::nullopt;
        }
        for (std::uint32_t index = 0; index < text.argument_count; ++index)
        {
            const auto argument = bounded_string(text.arguments[index]);
            if (!argument.has_value())
            {
                return std::nullopt;
            }
            const std::wstring token = L"{" + std::to_wstring(index) + L"}";
            std::size_t position{};
            while ((position = result.find(token, position)) != std::wstring::npos)
            {
                result.replace(position, token.size(), *argument);
                position += argument->size();
            }
        }
        return result;
    }

    BOOL WINAPI append_information_panel_entry(
        void* context,
        const glance::contracts::components::InformationPanelEntry* entry) noexcept
    {
        if (context == nullptr || entry == nullptr ||
            entry->size < sizeof(*entry))
        {
            return FALSE;
        }
        auto& collector = *static_cast<InformationPanelCollector*>(context);
        try
        {
            if (collector.cancelled->load(std::memory_order_acquire))
            {
                collector.valid = false;
                return FALSE;
            }
            auto label = resolve_information_panel_text(collector, entry->label);
            auto value = resolve_information_panel_text(collector, entry->value);
            if (!label.has_value() || label->empty() || !value.has_value())
            {
                collector.valid = false;
                return FALSE;
            }
            if (entry->kind ==
                glance::contracts::components::InformationPanelEntryKind::section)
            {
                if (!value->empty())
                {
                    collector.valid = false;
                    return FALSE;
                }
                if (!collector.text.empty())
                {
                    collector.text.append(L"\n\n");
                }
                collector.text.append(*label);
            }
            else if (entry->kind ==
                     glance::contracts::components::InformationPanelEntryKind::field)
            {
                if (value->empty())
                {
                    collector.valid = false;
                    return FALSE;
                }
                if (!collector.text.empty())
                {
                    collector.text.push_back(L'\n');
                }
                collector.text.append(*label);
                collector.text.append(L": ");
                collector.text.append(*value);
            }
            else
            {
                collector.valid = false;
                return FALSE;
            }
            if (collector.text.size() > 256 * 1024)
            {
                collector.valid = false;
                return FALSE;
            }
            return TRUE;
        }
        catch (...)
        {
            collector.valid = false;
            return FALSE;
        }
    }

    BOOL WINAPI information_panel_cancelled(void* context) noexcept
    {
        return context != nullptr &&
            static_cast<InformationPanelCollector*>(context)->cancelled->load(
                std::memory_order_acquire);
    }

    struct ImageMetadataCollector
    {
        std::vector<ImageMetadataEntry> entries;
        bool valid{ true };
    };

    BOOL WINAPI append_image_metadata(
        void* context,
        const ImageMetadataEntry* entry) noexcept
    {
        if (context == nullptr || entry == nullptr ||
            entry->size < sizeof(ImageMetadataEntry))
        {
            return FALSE;
        }
        auto& collector = *static_cast<ImageMetadataCollector*>(context);
        const auto canonical_length =
            wcsnlen_s(entry->canonical_name, std::size(entry->canonical_name));
        const auto text_length = wcsnlen_s(entry->text, std::size(entry->text));
        const bool valid_kind =
            entry->value_kind ==
                glance::contracts::components::ImageMetadataValueKind::text ||
            entry->value_kind ==
                glance::contracts::components::ImageMetadataValueKind::unsigned_integer ||
            entry->value_kind ==
                glance::contracts::components::ImageMetadataValueKind::floating_point ||
            entry->value_kind ==
                glance::contracts::components::ImageMetadataValueKind::timestamp;
        if (collector.entries.size() >= 128 ||
            canonical_length == std::size(entry->canonical_name) ||
            canonical_length < 8 ||
            std::wstring_view(entry->canonical_name, canonical_length).substr(0, 7) != L"System." ||
            text_length == std::size(entry->text) ||
            !valid_kind)
        {
            collector.valid = false;
            return FALSE;
        }
        try
        {
            collector.entries.push_back(*entry);
            return TRUE;
        }
        catch (...)
        {
            collector.valid = false;
            return FALSE;
        }
    }

    BOOL WINAPI image_metadata_cancelled(void* context) noexcept
    {
        return context == nullptr ||
            !static_cast<ImageMetadataCollector*>(context)->valid;
    }
}

namespace glance::app
{
    std::filesystem::path application_component_root()
    {
        return executable_directory() / L"components";
    }

    void initialize_components() noexcept
    {
        try
        {
            std::call_once(initialization_flag, initialize_registry);
        }
        catch (...)
        {
            glance::contracts::log_event(L"Component registry initialization failed.");
        }
    }

    bool component_has_extension(std::wstring_view extension) noexcept
    {
        try
        {
            initialize_components();
            const auto normalized = normalize_extension(extension);
            if (normalized.empty())
            {
                return false;
            }
            std::scoped_lock lock(registry_mutex);
            return extension_index.contains(normalized);
        }
        catch (...)
        {
            return false;
        }
    }

    GalleryMediaKind component_gallery_media_kind(std::wstring_view extension) noexcept
    {
        try
        {
            initialize_components();
            const auto normalized = normalize_extension(extension);
            if (normalized.empty())
            {
                return GalleryMediaKind::none;
            }
            std::scoped_lock lock(registry_mutex);
            const auto match = gallery_media_index.find(normalized);
            return match == gallery_media_index.end()
                ? GalleryMediaKind::none
                : match->second;
        }
        catch (...)
        {
            return GalleryMediaKind::none;
        }
    }

    std::vector<std::wstring> component_gallery_extensions(GalleryMediaKind kind) noexcept
    {
        try
        {
            initialize_components();
            std::scoped_lock lock(registry_mutex);
            const auto match = gallery_extension_index.find(kind);
            return match == gallery_extension_index.end()
                ? std::vector<std::wstring>{}
                : match->second;
        }
        catch (...)
        {
            return {};
        }
    }

    std::vector<ImageMetadataEntry> query_component_image_metadata(
        const std::shared_ptr<void>& lease_value) noexcept
    {
        if (lease_value == nullptr)
        {
            return {};
        }
        try
        {
            const auto lease = std::static_pointer_cast<PreviewLease>(lease_value);
            if (lease->component == nullptr || lease->token == 0 ||
                !lease->component->image_metadata.has_value())
            {
                return {};
            }
            ImageMetadataCollector collector;
            ImageMetadataSink sink{
                .context = &collector,
                .append = append_image_metadata,
                .is_cancelled = image_metadata_cancelled };
            if (!lease->component->image_metadata->query_metadata(lease->token, &sink) ||
                !collector.valid)
            {
                return {};
            }
            return collector.entries;
        }
        catch (...)
        {
            return {};
        }
    }

    ComponentLoadingMessage component_loading_text(
        const std::wstring& path,
        std::wstring_view language_tag) noexcept
    {
        try
        {
            initialize_components();
            static_cast<void>(language_tag);
            for (const auto& component : candidates_for_path(path))
            {
                if (component->api.can_preview(path.c_str()))
                {
                    return ComponentLoadingMessage{
                        .component_found = true,
                        .text = query_loading_text(*component, path) };
                }
            }
        }
        catch (...)
        {
        }
        return {};
    }

    ComponentPreviewResult prepare_component_preview(
        const std::wstring& path,
        std::wstring_view language_tag,
        glance::contracts::components::PreviewPreparationOptions options,
        glance::contracts::components::PreviewColorScheme color_scheme,
        const ComponentLoadingTextCallback& loading_callback,
        const std::shared_ptr<std::atomic_bool>& cancellation) noexcept
    {
        ComponentPreviewResult result;
        try
        {
            initialize_components();
            static_cast<void>(language_tag);
            const bool overridden = options.effective_extension[0] != L'\0';
            const auto dispatch_path = overridden ? L"preview" + std::wstring(options.effective_extension) : path;
            for (const auto& component : candidates_for_path(dispatch_path))
            {
                if (overridden ? (!component->api.can_preview_as || !component->api.can_preview_as(path.c_str(), &options))
                    : !component->api.can_preview(path.c_str()))
                {
                    continue;
                }
                if (loading_callback)
                {
                    try
                    {
                        loading_callback(query_loading_text(*component, path));
                    }
                    catch (...)
                    {
                    }
                }

                PreparedPreview preview;
                if (!component->progressive_preview.has_value() ||
                    !component->progressive_preview->refine_on_zoom)
                {
                    options.maximum_dimension =
                        glance::contracts::components::PreviewPreparationOptions{}.maximum_dimension;
                }
                options.size = sizeof(options);
                const glance::contracts::components::PreviewCancellation probe{
                    .context = cancellation.get(),
                    .is_cancelled = [](void* context) noexcept -> BOOL {
                        return context != nullptr && static_cast<std::atomic_bool*>(context)->load();
                    } };
                if (probe.is_cancelled(probe.context))
                {
                    result.status = glance::contracts::components::PrepareStatus::cancelled;
                    return result;
                }
                result.status = component->api.prepare_preview(path.c_str(), &options, &probe, &preview);
                if (const auto error_key = bounded_string(preview.error_key);
                    error_key.has_value() && !error_key->empty())
                {
                    result.error_detail =
                        localize_component_key(*component, *error_key);
                }
                if (result.status !=
                    glance::contracts::components::PrepareStatus::success)
                {
                    if (result.status ==
                        glance::contracts::components::PrepareStatus::unavailable)
                    {
                        continue;
                    }
                    return result;
                }

                result = materialize_preview(component, preview);
                if (result.status !=
                    glance::contracts::components::PrepareStatus::success)
                {
                    return result;
                }
                if (preview.kind == PreviewContentKind::web &&
                    preview.format == PreviewContentFormat::html &&
                    component->web_preview.has_value())
                {
                    result.web_preview = materialize_web_preview(
                        *component,
                        preview.lease_token,
                        color_scheme);
                    if (result.web_preview == nullptr)
                    {
                        result.status =
                            glance::contracts::components::PrepareStatus::failed;
                        return result;
                    }
                }
                const auto& notice = preview.notice;
                if (const auto notice_key = bounded_string(notice.text_key);
                    notice.size >= sizeof(notice) && notice_key.has_value() && !notice_key->empty())
                {
                    result.notice = localize_component_key(*component, *notice_key);
                    result.notice_severity = notice.severity;
                    result.notice_duration_ms = notice.duration_ms;
                }
                if (preview.lease_token != 0 &&
                    component->progressive_preview.has_value() &&
                    component->progressive_preview->can_refine(
                        preview.lease_token))
                {
                    auto session = std::make_shared<RefinementSession>();
                    session->component = component;
                    session->initial_lease = result.lease;
                    session->api = *component->progressive_preview;
                    session->cancellation = cancellation;
                    session->options = options;
                    session->kind = preview.kind;
                    session->format = preview.format;
                    session->token = preview.lease_token;
                    result.refinement_text =
                        query_refinement_text(*session);
                    result.refinement = std::move(session);
                    result.refinement_on_zoom = component->progressive_preview.has_value() &&
                        component->progressive_preview->refine_on_zoom;
                }
                return result;
            }
        }
        catch (...)
        {
            result.status = glance::contracts::components::PrepareStatus::failed;
        }
        return result;
    }

    ComponentPreviewResult refine_component_preview(
        const std::shared_ptr<void>& refinement,
        std::wstring_view language_tag) noexcept
    {
        ComponentPreviewResult result;
        if (refinement == nullptr)
        {
            result.status = glance::contracts::components::PrepareStatus::failed;
            return result;
        }
        try
        {
            const auto session =
                std::static_pointer_cast<RefinementSession>(refinement);
            PreparedPreview preview;
            static_cast<void>(language_tag);
            const glance::contracts::components::PreviewCancellation probe{
                .context = session->cancellation.get(),
                .is_cancelled = [](void* context) noexcept -> BOOL {
                    return context != nullptr && static_cast<std::atomic_bool*>(context)->load();
                } };
            result.status = session->api.prepare_refined_preview(
                session->token, &session->options, &probe, &preview);
            if (const auto error_key = bounded_string(preview.error_key);
                error_key.has_value() && !error_key->empty())
            {
                result.error_detail =
                    localize_component_key(*session->component, *error_key);
            }
            if (result.status !=
                glance::contracts::components::PrepareStatus::success)
            {
                return result;
            }
            if (preview.kind != session->kind || preview.format != session->format)
            {
                session->component->api.release_preview(preview.lease_token);
                result.status =
                    glance::contracts::components::PrepareStatus::failed;
                return result;
            }
            return materialize_preview(session->component, preview);
        }
        catch (...)
        {
            result.status = glance::contracts::components::PrepareStatus::failed;
            return result;
        }
    }

    glance::contracts::components::FileDirectoryOpenStatus
        open_component_file_directory(
            const std::shared_ptr<void>& session_value,
            std::wstring_view language_tag,
            std::wstring_view password,
            FileDirectoryDescriptor& descriptor) noexcept
    {
        using namespace glance::contracts::components;
        descriptor = {};
        if (session_value == nullptr)
        {
            return FileDirectoryOpenStatus::failed;
        }
        try
        {
            const auto session = std::static_pointer_cast<FileDirectorySession>(session_value);
            glance::contracts::components::FileDirectoryDescriptor native;
            static_cast<void>(language_tag);
            const std::wstring password_value(password);
            const auto status = session->api.open(
                session->token,
                password_value.c_str(),
                &native);
            if (status != FileDirectoryOpenStatus::ready)
            {
                return status;
            }
            if (native.size < sizeof(native) ||
                (native.presentation != FileDirectoryPresentation::list &&
                 native.presentation != FileDirectoryPresentation::tree) ||
                native.info_field_count > maximum_file_directory_info_fields ||
                native.column_count == 0 ||
                native.column_count > maximum_file_directory_columns)
            {
                return FileDirectoryOpenStatus::failed;
            }

            descriptor.presentation = native.presentation;
            descriptor.truncated = native.truncated != FALSE;
            descriptor.depth_limited = native.depth_limited != FALSE;
            descriptor.info_fields.reserve(native.info_field_count);
            std::unordered_set<std::wstring> info_ids;
            for (std::uint32_t index = 0; index < native.info_field_count; ++index)
            {
                const auto& field = native.info_fields[index];
                const auto id = bounded_string(field.id);
                const auto label_key = bounded_string(field.label_key);
                const auto value = bounded_string(field.text);
                if (!id.has_value() || !valid_setting_id(*id) ||
                    !info_ids.insert(*id).second ||
                    !label_key.has_value() || !valid_resource_key(*label_key) ||
                    !value.has_value() ||
                    !valid_file_directory_value_kind(field.kind))
                {
                    return FileDirectoryOpenStatus::failed;
                }
                descriptor.info_fields.push_back(FileDirectoryInfoField{
                    .id = *id,
                    .label = localize_component_key(
                        *session->component,
                        *label_key),
                    .value = FileDirectoryValue{
                        .kind = field.kind,
                        .unsigned_value = field.unsigned_value,
                        .ratio_value = field.ratio_value,
                        .text = field.text_kind ==
                                glance::contracts::components::ComponentTextKind::resource_key
                            ? localize_component_key(*session->component, *value)
                            : *value } });
                if (field.text_kind !=
                        glance::contracts::components::ComponentTextKind::literal &&
                    field.text_kind !=
                        glance::contracts::components::ComponentTextKind::resource_key)
                {
                    return FileDirectoryOpenStatus::failed;
                }
            }
            descriptor.columns.reserve(native.column_count);
            std::unordered_set<std::wstring> column_ids;
            for (std::uint32_t index = 0; index < native.column_count; ++index)
            {
                const auto& column = native.columns[index];
                const auto id = bounded_string(column.id);
                const auto title_key = bounded_string(column.title_key);
                if (!id.has_value() || !valid_setting_id(*id) ||
                    !column_ids.insert(*id).second ||
                    !title_key.has_value() || !valid_resource_key(*title_key) ||
                    !valid_file_directory_value_kind(column.kind) ||
                    column.kind == FileDirectoryValueKind::none ||
                    (column.alignment != FileDirectoryAlignment::left &&
                     column.alignment != FileDirectoryAlignment::right) ||
                    column.width > 1000)
                {
                    return FileDirectoryOpenStatus::failed;
                }
                descriptor.columns.push_back(FileDirectoryColumn{
                    .id = *id,
                    .title = localize_component_key(
                        *session->component,
                        *title_key),
                    .kind = column.kind,
                    .alignment = column.alignment,
                    .width = column.width,
                    .sortable = column.sortable != FALSE });
            }
            if (descriptor.columns.front().id != L"name" ||
                descriptor.columns.front().kind != FileDirectoryValueKind::text)
            {
                descriptor = {};
                return FileDirectoryOpenStatus::failed;
            }
            return status;
        }
        catch (...)
        {
            descriptor = {};
            return FileDirectoryOpenStatus::failed;
        }
    }

    FileDirectoryPage enumerate_component_file_directory(
        const std::shared_ptr<void>& session_value,
        std::uint64_t parent_node_id,
        std::uint32_t offset,
        std::uint32_t limit) noexcept
    {
        FileDirectoryPage result;
        if (session_value == nullptr || limit == 0 || limit > 128)
        {
            result.failed = true;
            return result;
        }
        try
        {
            const auto session = std::static_pointer_cast<FileDirectorySession>(session_value);
            result.entries.reserve(limit);
            FileDirectoryEntryCollector collector{
                .component = session->component.get(),
                .entries = &result.entries };
            glance::contracts::components::FileDirectoryEntrySink sink{
                .context = &collector,
                .append = append_file_directory_entry };
            std::uint32_t returned{};
            if (!session->api.enumerate_children(
                    session->token,
                    parent_node_id,
                    offset,
                    limit,
                    &sink,
                    &returned,
                    &result.total) ||
                !collector.valid || returned != result.entries.size() ||
                returned > limit ||
                static_cast<std::uint64_t>(result.total) <
                    static_cast<std::uint64_t>(offset) + returned)
            {
                result.entries.clear();
                result.failed = true;
            }
        }
        catch (...)
        {
            result.entries.clear();
            result.failed = true;
        }
        return result;
    }

    std::vector<ComponentStatus> component_statuses(
        std::wstring_view language_tag) noexcept
    {
        std::vector<std::shared_ptr<LoadedComponent>> components;
        {
            initialize_components();
            std::scoped_lock lock(registry_mutex);
            components = registered_components;
        }

        std::vector<ComponentStatus> statuses;
        static_cast<void>(language_tag);
        for (const auto& component : components)
        {
            try
            {
                glance::contracts::components::ComponentStatusResult status;
                if (!component->api.query_status(
                        &status) ||
                    status.display_name_key[0] == L'\0')
                {
                    continue;
                }
                const auto display_name_key = bounded_string(status.display_name_key);
                const auto detail_key = bounded_string(status.detail_key);
                if (!display_name_key.has_value() ||
                    !valid_resource_key(*display_name_key) ||
                    !detail_key.has_value() ||
                    (!detail_key->empty() && !valid_resource_key(*detail_key)))
                {
                    continue;
                }
                ComponentState state = ComponentState::error;
                if (status.severity == HealthSeverity::healthy)
                {
                    state = ComponentState::healthy;
                }
                else if (status.severity == HealthSeverity::warning)
                {
                    state = ComponentState::warning;
                }
                std::wstring detail = detail_key->empty()
                    ? L""
                    : localize_component_key(*component, *detail_key);
                if (!component->active &&
                    component->dependency_failure != DependencyFailure::none)
                {
                    state = ComponentState::error;
                    if (component->dependency_failure == DependencyFailure::cycle)
                    {
                        detail = glance::app::localize(L"ComponentDependencyCycle");
                    }
                    else
                    {
                        detail = glance::app::localize_format(
                            L"ComponentDependencyUnavailable",
                            { component->dependency_name });
                    }
                }
                else if (component->registration.preferred_kind ==
                        PreviewContentKind::web &&
                    component->web_preview.has_value() &&
                    !glance::app::webview_runtime_available())
                {
                    state = ComponentState::error;
                    detail = glance::app::localize(L"ComponentWebViewUnavailable");
                }
                statuses.push_back(ComponentStatus{
                    .id = component->id,
                    .display_name =
                        localize_component_key(*component, *display_name_key),
                    .detail = std::move(detail),
                    .state = state,
                    .metadata = {localize_component_key(*component, component->metadata_keys[0]),
                        localize_component_key(*component, component->metadata_keys[1]),
                        localize_component_key(*component, component->metadata_keys[2]), component} });
            }
            catch (...)
            {
            }
        }
        std::ranges::sort(statuses, [](const auto& left, const auto& right) {
            const auto comparison = CompareStringOrdinal(
                left.display_name.c_str(),
                -1,
                right.display_name.c_str(),
                -1,
                TRUE);
            return comparison == CSTR_EQUAL
                ? left.id < right.id
                : comparison == CSTR_LESS_THAN;
        });
        return statuses;
    }

    std::vector<ComponentStatusBarShortcut> component_status_bar_shortcuts(
        std::wstring_view path,
        PreviewContentKind kind,
        PreviewContentFormat format,
        std::wstring_view language_tag) noexcept
    {
        std::vector<std::shared_ptr<LoadedComponent>> components;
        {
            initialize_components();
            std::scoped_lock lock(registry_mutex);
            components = registered_components;
        }

        std::vector<ComponentStatusBarShortcut> shortcuts;
        const std::wstring source(path);
        static_cast<void>(language_tag);
        for (const auto& component : components)
        {
            if (!component->active || !component->status_bar_shortcut.has_value())
            {
                continue;
            }
            try
            {
                std::uint32_t count{};
                if (!component->status_bar_shortcut->enumerate_shortcuts(
                        nullptr, 0, &count) ||
                    count == 0 ||
                    count > glance::contracts::components::maximum_status_bar_shortcuts)
                {
                    continue;
                }
                std::vector<glance::contracts::components::StatusBarShortcutDescriptor>
                    descriptors(count);
                std::uint32_t written = count;
                if (!component->status_bar_shortcut->enumerate_shortcuts(
                        descriptors.data(), count, &written) ||
                    written != count)
                {
                    continue;
                }
                for (const auto& descriptor : descriptors)
                {
                    const auto shortcut_id = bounded_string(descriptor.shortcut_id);
                    const auto tooltip_key = bounded_string(descriptor.tooltip_key);
                    if (descriptor.size < sizeof(descriptor) ||
                        !shortcut_id.has_value() || !valid_setting_id(*shortcut_id) ||
                        !tooltip_key.has_value() ||
                        !valid_resource_key(*tooltip_key) ||
                        descriptor.target_kind != kind ||
                        (descriptor.target_format != PreviewContentFormat::none &&
                         descriptor.target_format != format) ||
                        descriptor.fluent_icon_glyph < 0xe000 ||
                        descriptor.fluent_icon_glyph > 0xf8ff)
                    {
                        continue;
                    }
                    const auto state = component->status_bar_shortcut->query_state(
                        shortcut_id->c_str(), source.c_str(), kind, format);
                    if (state == glance::contracts::components::StatusBarShortcutState::hidden)
                    {
                        continue;
                    }
                    if (state != glance::contracts::components::StatusBarShortcutState::ready &&
                        state != glance::contracts::components::StatusBarShortcutState::setup_required)
                    {
                        continue;
                    }
                    shortcuts.push_back(ComponentStatusBarShortcut{
                        .component_id = component->id,
                        .shortcut_id = std::move(*shortcut_id),
                        .tooltip = localize_component_key(
                            *component,
                            *tooltip_key),
                        .order = descriptor.order,
                        .fluent_icon_glyph = descriptor.fluent_icon_glyph,
                        .state = state == glance::contracts::components::StatusBarShortcutState::ready
                            ? ComponentStatusBarShortcutState::ready
                            : ComponentStatusBarShortcutState::setup_required,
                        .initially_checked = descriptor.initially_checked != FALSE,
                        .supports_data_copy =
                            state == glance::contracts::components::StatusBarShortcutState::ready &&
                            component->information_provider.has_value() &&
                            component->information_provider->query_json != nullptr,
                        .lease = std::static_pointer_cast<void>(component) });
                }
            }
            catch (...)
            {
            }
        }
        std::ranges::sort(shortcuts, [](const auto& left, const auto& right) {
            return std::tie(left.order, left.component_id, left.shortcut_id) <
                std::tie(right.order, right.component_id, right.shortcut_id);
        });
        return shortcuts;
    }

    ComponentStatusBarActivation activate_component_status_bar_shortcut(
        const ComponentStatusBarShortcut& shortcut,
        std::wstring_view path,
        std::wstring_view language_tag,
        bool requested_checked) noexcept
    {
        ComponentStatusBarActivation activation;
        try
        {
            const auto component =
                std::static_pointer_cast<LoadedComponent>(shortcut.lease);
            if (component == nullptr || !component->active ||
                component->id != shortcut.component_id ||
                !component->status_bar_shortcut.has_value())
            {
                return activation;
            }
            glance::contracts::components::StatusBarShortcutActivationResult result;
            const std::wstring source(path);
            static_cast<void>(language_tag);
            if (!component->status_bar_shortcut->activate(
                    shortcut.shortcut_id.c_str(),
                    source.c_str(),
                    requested_checked ? TRUE : FALSE,
                    &result) ||
                result.size < sizeof(result))
            {
                return activation;
            }
            const auto hover_info_id = bounded_string(result.hover_info_id);
            const auto dependency_id = bounded_string(result.dependency_id);
            const auto loading_text_key = bounded_string(result.loading_text_key);
            if (!hover_info_id.has_value() || !dependency_id.has_value() ||
                !loading_text_key.has_value())
            {
                return activation;
            }
            if (result.activation ==
                    glance::contracts::components::StatusBarShortcutActivation::toggle_hover_info &&
                result.checked && valid_setting_id(*hover_info_id) &&
                component->information_provider.has_value() &&
                component->information_provider->query_info != nullptr)
            {
                activation.kind = ComponentStatusBarActivationKind::toggle_hover_info;
                activation.checked = true;
                activation.hover_info_id = std::move(*hover_info_id);
                activation.loading_text = loading_text_key->empty()
                    ? L""
                    : localize_component_key(*component, *loading_text_key);
            }
            else if (result.activation ==
                         glance::contracts::components::StatusBarShortcutActivation::toggle_hover_info &&
                !result.checked)
            {
                activation.kind = ComponentStatusBarActivationKind::toggle_hover_info;
            }
            else if (result.activation == glance::contracts::components::StatusBarShortcutActivation::request_dependency &&
                valid_setting_id(*dependency_id))
            {
                activation.kind = ComponentStatusBarActivationKind::request_dependency;
                activation.dependency_id = *dependency_id;
            }
            else if (result.activation == glance::contracts::components::
                         StatusBarShortcutActivation::set_native_media_view_mode)
            {
                activation.kind = ComponentStatusBarActivationKind::set_native_media_view_mode;
            }
            activation.checked = result.checked != FALSE;
            activation.component_id = component->id;
            activation.lease = std::static_pointer_cast<void>(component);
        }
        catch (...)
        {
        }
        return activation;
    }

    bool component_can_preview_as(const std::wstring& path, const std::wstring& extension) noexcept
    {
        try
        {
            glance::contracts::components::PreviewPreparationOptions options;
            if (extension.size() >= std::size(options.effective_extension)) return false;
            wcscpy_s(options.effective_extension, extension.c_str());
            for (const auto& component : candidates_for_path(L"preview" + extension))
                if (component->api.can_preview_as && component->api.can_preview_as(path.c_str(), &options)) return true;
        }
        catch (...) {}
        return false;
    }

    bool has_file_format_identifier() noexcept
    {
        std::scoped_lock lock(registry_mutex);
        return std::ranges::any_of(registered_components, [](const auto& component) {
            return component->active && component->information_provider &&
                component->information_provider->identify_format;
        });
    }

    FileFormatIdentification identify_file_format(const std::wstring& path, const std::atomic_bool& cancelled) noexcept
    {
        using namespace glance::contracts::components;
        FileFormatIdentification result;
        try
        {
            std::vector<std::shared_ptr<LoadedComponent>> components;
            {
                std::scoped_lock lock(registry_mutex);
                components = registered_components;
            }
            for (const auto& component : components)
            {
                if (cancelled.load() || !component->active || !component->information_provider ||
                    !component->information_provider->identify_format) continue;
                InformationPanelCollector collector{.component = component.get(), .cancelled = &cancelled};
                InformationPanelSink information{.context = &collector, .append = append_information_panel_entry,
                    .is_cancelled = information_panel_cancelled};
                FileFormatSink sink{.context = &result, .append = [](void* context, const FileFormatCandidate* candidate) noexcept -> BOOL {
                    try
                    {
                        if (!candidate || !bounded_string(candidate->name) || !bounded_string(candidate->version) ||
                            !bounded_string(candidate->mime) || !bounded_string(candidate->identifier) ||
                            !bounded_string(candidate->basis) || !bounded_string(candidate->extensions)) return FALSE;
                        auto& target = *static_cast<FileFormatIdentification*>(context);
                        if (target.candidates.size() >= 16) return FALSE;
                        target.candidates.push_back(*candidate);
                        return TRUE;
                    }
                    catch (...) { return FALSE; }
                }};
                PreviewCancellation cancellation{.context = const_cast<std::atomic_bool*>(&cancelled),
                    .is_cancelled = [](void* context) noexcept -> BOOL { return static_cast<std::atomic_bool*>(context)->load(); }};
                const auto status = component->information_provider->identify_format(path.c_str(), &cancellation, &information, &sink);
                if (status == PrepareStatus::success && collector.valid && !cancelled.load())
                {
                    result.status = std::move(collector.text);
                    return result;
                }
                result = {};
            }
        }
        catch (...) {}
        return result;
    }

    std::wstring query_component_hover_info(
        const ComponentStatusBarActivation& activation,
        std::wstring_view path,
        std::wstring_view language_tag,
        const std::atomic_bool& cancelled) noexcept
    {
        try
        {
            const auto component =
                std::static_pointer_cast<LoadedComponent>(activation.lease);
            if (component == nullptr || !component->active ||
                component->id != activation.component_id ||
                !component->information_provider.has_value() ||
                component->information_provider->query_info == nullptr ||
                !valid_setting_id(activation.hover_info_id))
            {
                return {};
            }
            static_cast<void>(language_tag);
            InformationPanelCollector collector{
                .component = component.get(),
                .cancelled = &cancelled };
            glance::contracts::components::InformationPanelSink sink{
                .context = &collector,
                .append = append_information_panel_entry,
                .is_cancelled = information_panel_cancelled };
            const std::wstring source(path);
            const auto status = component->information_provider->query_info(
                activation.hover_info_id.c_str(),
                source.c_str(),
                &sink);
            return status == glance::contracts::components::PrepareStatus::success &&
                    collector.valid && !cancelled.load(std::memory_order_acquire)
                ? std::move(collector.text)
                : std::wstring{};
        }
        catch (...)
        {
            return {};
        }
    }

    std::wstring query_component_status_bar_shortcut_data(
        const ComponentStatusBarShortcut& shortcut,
        std::wstring_view path,
        const std::atomic_bool& cancelled) noexcept
    {
        try
        {
            const auto component =
                std::static_pointer_cast<LoadedComponent>(shortcut.lease);
            if (component == nullptr || !component->active ||
                component->id != shortcut.component_id ||
                !shortcut.supports_data_copy ||
                !component->information_provider.has_value() ||
                component->information_provider->query_json == nullptr ||
                !valid_setting_id(shortcut.shortcut_id))
            {
                return {};
            }
            HoverInfoCollector collector{ .cancelled = &cancelled };
            glance::contracts::components::HoverInfoTextSink sink{
                .context = &collector,
                .append = append_hover_info,
                .is_cancelled = hover_info_cancelled };
            const std::wstring source(path);
            const auto status = component->information_provider->query_json(
                shortcut.shortcut_id.c_str(),
                source.c_str(),
                &sink);
            return status == glance::contracts::components::PrepareStatus::success &&
                    collector.valid && !cancelled.load(std::memory_order_acquire)
                ? std::move(collector.text)
                : std::wstring{};
        }
        catch (...)
        {
            return {};
        }
    }



    std::optional<PagedDocumentRendererRegistration>
    paged_document_renderer() noexcept
    {
        try
        {
            initialize_components();
            std::vector<std::shared_ptr<LoadedComponent>> candidates;
            {
                std::scoped_lock lock(registry_mutex);
                const auto match = renderer_index.find(renderer_key(
                    PreviewContentKind::document,
                    PreviewContentFormat::pdf));
                if (match == renderer_index.end())
                {
                    return std::nullopt;
                }
                candidates = match->second;
            }
            for (const auto& component : candidates)
            {
                if (!component->paged_document_host.has_value())
                {
                    continue;
                }
                return PagedDocumentRendererRegistration{
                    .host_path = component->paged_document_host->wstring(),
                    .lease = std::static_pointer_cast<void>(component) };
            }
        }
        catch (...)
        {
        }
        return std::nullopt;
    }

    std::vector<ComponentSettingsRegistration> component_settings_registrations()
    {
        using namespace glance::contracts::components;
        std::vector<std::shared_ptr<LoadedComponent>> components;
        {
            initialize_components();
            std::scoped_lock lock(registry_mutex);
            components = registered_components;
        }
        std::vector<ComponentSettingsRegistration> result;
        for (const auto& component : components)
        {
            if (!component->active || !component->settings_contribution) continue;
            struct Transaction
            {
                ComponentSettingsRegistration result;
                std::map<std::wstring, bool> parents{
                    {L"general", true}, {L"window", true}, {L"footer", true},
                    {L"text", true}, {L"media", true}, {L"components", true}, {L"maintenance", true}};
                std::set<std::wstring> ids;
                bool failed{};
            } transaction;
            transaction.result.component_id = component->id;
            transaction.result.lease = component;
            const auto append = []<typename T>(void* context, const T* value) noexcept -> BOOL {
                auto& transaction = *static_cast<Transaction*>(context);
                try
                {
                    if (value == nullptr || value->size < sizeof(T) || transaction.result.entries.size() >= 256)
                        throw std::invalid_argument("Invalid component settings descriptor");
                    const auto id = [&] {
                        if constexpr (std::is_same_v<T, ComponentSettingDescriptor>) return bounded_string(value->setting_id);
                        else return bounded_string(value->id);
                    }();
                    if (!id || !valid_setting_id(*id) || transaction.parents.contains(*id) || !transaction.ids.insert(*id).second)
                        throw std::invalid_argument("Duplicate component settings ID");
                    if constexpr (std::is_same_v<T, SettingsPageDescriptor>)
                    {
                        if (!bounded_string(value->icon) || !bounded_string(value->description_key) ||
                            !bounded_string(value->name_key) || !valid_resource_key(value->name_key))
                            throw std::invalid_argument("Invalid component settings page");
                        transaction.parents.emplace(*id, true);
                    }
                    else
                    {
                        const auto parent = [&] {
                            if constexpr (std::is_same_v<T, SettingsSectionDescriptor>) return bounded_string(value->page);
                            else return bounded_string(value->parent);
                        }();
                        if (!parent || !transaction.parents.contains(*parent))
                            throw std::invalid_argument("Unknown component settings parent");
                        if constexpr (std::is_same_v<T, SettingsSectionDescriptor>)
                        {
                            if (!transaction.parents.at(*parent) || !bounded_string(value->name_key) ||
                                !valid_resource_key(value->name_key) || !bounded_string(value->description_key))
                                throw std::invalid_argument("Invalid component settings section");
                            transaction.parents.emplace(*id, false);
                        }
                        else if constexpr (std::is_same_v<T, SettingsCustomItemDescriptor>)
                        {
                            if (!value->create || !value->refresh || !value->close)
                                throw std::invalid_argument("Incomplete component settings view");
                        }
                        else
                        {
                            if (!bounded_string(value->icon) || !bounded_string(value->label_key) || !valid_resource_key(value->label_key) ||
                                !bounded_string(value->description_key) || !bounded_string(value->enabled_description_key) ||
                                !bounded_string(value->disabled_description_key) || !bounded_string(value->row_id) ||
                                !bounded_string(value->row_title_key) || value->option_count > maximum_setting_options ||
                                (value->kind != ComponentSettingKind::toggle && value->kind != ComponentSettingKind::choice &&
                                 value->kind != ComponentSettingKind::number))
                                throw std::invalid_argument("Invalid component setting");
                            if (value->kind == ComponentSettingKind::number &&
                                (value->minimum_value > value->default_value || value->default_value > value->maximum_value ||
                                 value->small_change <= 0 || value->decimal_places > 3))
                                throw std::invalid_argument("Invalid component number range");
                            if (value->kind == ComponentSettingKind::choice && value->option_count == 0)
                                throw std::invalid_argument("Empty component choices");
                            for (std::uint32_t index = 0; index < value->option_count; ++index)
                                if (!bounded_string(value->options[index].text_key) || !valid_resource_key(value->options[index].text_key))
                                    throw std::invalid_argument("Invalid component choice label");
                        }
                    }
                    transaction.result.entries.emplace_back(*value);
                    return TRUE;
                }
                catch (...)
                {
                    transaction.failed = true;
                    return FALSE;
                }
            };
            const SettingsRegistrar registrar{
                .context = &transaction,
                .register_page = static_cast<BOOL(WINAPI*)(void*, const SettingsPageDescriptor*) noexcept>(append),
                .register_section = static_cast<BOOL(WINAPI*)(void*, const SettingsSectionDescriptor*) noexcept>(append),
                .register_item = static_cast<BOOL(WINAPI*)(void*, const ComponentSettingDescriptor*) noexcept>(append),
                .register_custom_item = static_cast<BOOL(WINAPI*)(void*, const SettingsCustomItemDescriptor*) noexcept>(append) };
            if (component->settings_contribution->register_settings(&registrar) && !transaction.failed)
                result.push_back(std::move(transaction.result));
            else contracts::log_event(L"Component settings registration failed: " + component->id);
        }
        return result;
    }

    std::vector<ComponentSetting> component_settings(std::wstring_view language_tag) noexcept
    {
        using namespace glance::contracts::components;
        static_cast<void>(language_tag);
        std::vector<ComponentSetting> result;
        try
        {
            for (const auto& registration : component_settings_registrations())
            {
                for (const auto& entry : registration.entries)
                {
                    const auto* descriptor = std::get_if<ComponentSettingDescriptor>(&entry);
                    if (!descriptor) continue;
                    ComponentSetting setting;
                    setting.component_id = registration.component_id;
                    setting.setting_id = descriptor->setting_id;
                    setting.label = localize_component(registration.component_id, descriptor->label_key);
                    setting.kind = descriptor->kind;
                    setting.default_value = descriptor->default_value;
                    setting.minimum_value = descriptor->minimum_value;
                    setting.maximum_value = descriptor->maximum_value;
                    setting.small_change = descriptor->small_change;
                    setting.decimal_places = descriptor->decimal_places;
                    for (std::uint32_t index = 0; index < descriptor->option_count; ++index)
                        setting.options.push_back({descriptor->options[index].value,
                            localize_component(registration.component_id, descriptor->options[index].text_key)});
                    result.push_back(std::move(setting));
                }
            }
        }
        catch (...) { contracts::log_event(L"Component settings query failed"); }
        return result;
    }

    std::int64_t component_setting_value(
        std::wstring_view component_id,
        std::wstring_view setting_id,
        std::int64_t default_value) noexcept
    {
        if (!valid_component_id(component_id) || !valid_setting_id(setting_id))
        {
            return default_value;
        }
        try
        {
            const std::wstring key_path = L"Software\\Glance\\Components\\" +
                std::wstring(component_id);
            ULONGLONG value{};
            DWORD size = sizeof(value);
            if (contracts::storage::read_value(key_path, setting_id, REG_QWORD, &value, &size) == ERROR_SUCCESS && size == sizeof(value))
            {
                return static_cast<std::int64_t>(value);
            }

            if (component_id == L"pdf" && setting_id == L"render-dimension")
            {
                DWORD legacy_value{};
                size = sizeof(legacy_value);
                if (contracts::storage::read_value(L"MediaPreview", L"PdfPreviewRenderDimension", REG_DWORD,
                    &legacy_value, &size) == ERROR_SUCCESS && size == sizeof(legacy_value))
                {
                    save_component_setting_value(component_id, setting_id, legacy_value);
                    return legacy_value;
                }
            }
        }
        catch (...)
        {
        }
        return default_value;
    }

    void save_component_setting_value(
        std::wstring_view component_id,
        std::wstring_view setting_id,
        std::int64_t value) noexcept
    {
        if (!valid_component_id(component_id) || !valid_setting_id(setting_id))
        {
            return;
        }
        try
        {
            const std::wstring key_path = L"Software\\Glance\\Components\\" +
                std::wstring(component_id);
            contracts::storage::Batch key(key_path, contracts::storage::settings());
            const auto stored = static_cast<ULONGLONG>(value);
            key.set(setting_id, REG_QWORD, &stored, sizeof(stored));
            static_cast<void>(key.commit());
        }
        catch (...)
        {
        }
    }

    std::vector<std::pair<std::wstring, std::int64_t>> component_setting_values(
        std::wstring_view component_id) noexcept
    {
        std::vector<std::pair<std::wstring, std::int64_t>> values;
        if (!valid_component_id(component_id))
        {
            return values;
        }
        for (const auto& setting : component_settings({}))
        {
            if (setting.component_id == component_id)
            {
                values.emplace_back(
                    setting.setting_id,
                    component_setting_value(
                        setting.component_id,
                        setting.setting_id,
                        setting.default_value));
            }
        }
        return values;
    }

    void shutdown_components() noexcept
    {
        std::vector<std::shared_ptr<LoadedComponent>> components;
        {
            std::scoped_lock lock(registry_mutex);
            extension_index.clear();
            renderer_index.clear();
            gallery_media_index.clear();
            gallery_extension_index.clear();
            components = std::move(registered_components);
        }
        components.clear();
    }
}

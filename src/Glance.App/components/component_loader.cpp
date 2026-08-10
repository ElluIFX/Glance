#include "pch.h"
#include "component_loader.h"

#include "localization.h"
#include "webview_availability.h"
#include "glance/contracts/diagnostics.h"
#include "../../version.h"

#include <algorithm>
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
    using glance::contracts::components::ConfigurablePreviewApi;
    using glance::contracts::components::GetApiFunction;
    using glance::contracts::components::GalleryMediaApi;
    using glance::contracts::components::GalleryMediaKind;
    using glance::contracts::components::HealthSeverity;
    using glance::contracts::components::FileDirectoryPreviewApi;
    using glance::contracts::components::PreparedPreview;
    using glance::contracts::components::PagedDocumentHostDescriptor;
    using glance::contracts::components::PagedDocumentRendererApi;
    using glance::contracts::components::PreviewContentFormat;
    using glance::contracts::components::PreviewContentKind;
    using glance::contracts::components::PreviewNoticeApi;
    using glance::contracts::components::ProgressivePreviewApi;
    using glance::contracts::components::SettingsContributionApi;
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
        std::optional<ConfigurablePreviewApi> configurable_preview;
        std::optional<ProgressivePreviewApi> progressive_preview;
        std::optional<PreviewNoticeApi> preview_notice;
        std::optional<WebPreviewApi> web_preview;
        std::optional<PagedDocumentRendererApi> paged_document_renderer;
        std::optional<std::filesystem::path> paged_document_host;
        std::optional<SettingsContributionApi> settings_contribution;
        std::optional<FileDirectoryPreviewApi> file_directory_preview;
        std::optional<GalleryMediaApi> gallery_media;
        std::vector<std::wstring> extensions;
        std::vector<std::wstring> dependencies;
        std::vector<RendererRegistration> renderers;
        std::wstring dependency_name;
        DependencyFailure dependency_failure{ DependencyFailure::none };
        bool activation_ready{ true };
        bool active{};

        ~LoadedComponent()
        {
            if (api.shutdown != nullptr)
            {
                api.shutdown();
            }
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
            return format == PreviewContentFormat::pdf;
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

    BOOL WINAPI register_extension(void* context, const wchar_t* extension) noexcept
    {
        if (context == nullptr || extension == nullptr)
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
            component->api.can_preview == nullptr ||
            component->api.prepare_preview == nullptr ||
            component->api.release_preview == nullptr ||
            component->api.query_interface == nullptr ||
            component->api.shutdown == nullptr)
        {
            return {};
        }

        RegistrationCollector collector;
        glance::contracts::components::ComponentRegistrar registrar{
            .context = &collector,
            .register_extension = register_extension,
            .register_renderer = register_renderer };
        if (!component->api.initialize(&registrar, &component->registration) ||
            component->registration.size < sizeof(ComponentRegistration) ||
            manifest.id != component->registration.component_id ||
            std::wstring_view(component->registration.target_app_version) !=
                GLANCE_VERSION_WSTRING ||
            collector.extensions.empty() ||
            !valid_content_pair(
                component->registration.preferred_kind,
                component->registration.preferred_format))
        {
            return {};
        }
        component->id = manifest.id;
        component->extensions = std::move(collector.extensions);
        component->renderers = std::move(collector.renderers);
        void* interface_pointer{};
        if (component->api.query_interface(
                &glance::contracts::components::configurable_preview_api_id,
                glance::contracts::components::configurable_preview_api_version,
                &interface_pointer) &&
            interface_pointer != nullptr)
        {
            const auto* interface_api =
                static_cast<const ConfigurablePreviewApi*>(interface_pointer);
            if (interface_api->size >= sizeof(ConfigurablePreviewApi) &&
                interface_api->version ==
                    glance::contracts::components::configurable_preview_api_version &&
                interface_api->prepare_preview != nullptr)
            {
                component->configurable_preview = *interface_api;
            }
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
                &glance::contracts::components::preview_notice_api_id,
                glance::contracts::components::preview_notice_api_version,
                &interface_pointer) &&
            interface_pointer != nullptr)
        {
            const auto* interface_api =
                static_cast<const PreviewNoticeApi*>(interface_pointer);
            if (interface_api->size >= sizeof(PreviewNoticeApi) &&
                interface_api->version ==
                    glance::contracts::components::preview_notice_api_version &&
                interface_api->query_preview_notice != nullptr)
            {
                component->preview_notice = *interface_api;
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
                &glance::contracts::components::paged_document_renderer_api_id,
                glance::contracts::components::paged_document_renderer_api_version,
                &interface_pointer) &&
            interface_pointer != nullptr)
        {
            const auto* interface_api =
                static_cast<const PagedDocumentRendererApi*>(interface_pointer);
            if (interface_api->size >= sizeof(PagedDocumentRendererApi) &&
                interface_api->version ==
                    glance::contracts::components::paged_document_renderer_api_version &&
                interface_api->query_host != nullptr)
            {
                component->paged_document_renderer = *interface_api;
            }
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
                interface_api->enumerate_settings != nullptr)
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
                &glance::contracts::components::gallery_media_api_id,
                glance::contracts::components::gallery_media_api_version,
                &interface_pointer) &&
            interface_pointer != nullptr)
        {
            const auto* interface_api =
                static_cast<const GalleryMediaApi*>(interface_pointer);
            if (interface_api->size >= sizeof(GalleryMediaApi) &&
                interface_api->version ==
                    glance::contracts::components::gallery_media_api_version &&
                interface_api->classify_extension != nullptr)
            {
                component->gallery_media = *interface_api;
            }
        }
        if (std::ranges::any_of(component->renderers, [&component](const auto& renderer) {
                if (IsEqualGUID(
                        renderer.interface_id,
                        glance::contracts::components::paged_document_renderer_api_id))
                {
                    return !component->paged_document_renderer.has_value() ||
                        renderer.interface_version !=
                            glance::contracts::components::paged_document_renderer_api_version;
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
        if (component->paged_document_renderer.has_value())
        {
            PagedDocumentHostDescriptor descriptor;
            const bool described =
                component->paged_document_renderer->query_host(&descriptor) != FALSE &&
                descriptor.size >= sizeof(PagedDocumentHostDescriptor);
            const auto executable = described
                ? bounded_string(descriptor.host_executable)
                : std::nullopt;
            const std::filesystem::path relative = executable.has_value()
                ? std::filesystem::path(*executable)
                : std::filesystem::path{};
            std::error_code error;
            component->activation_ready = executable.has_value() &&
                !relative.empty() && relative == relative.filename() &&
                _wcsicmp(relative.extension().c_str(), L".exe") == 0 &&
                std::filesystem::is_regular_file(directory / relative, error);
            if (component->activation_ready)
            {
                component->paged_document_host = directory / relative;
            }
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
                if (!component->gallery_media.has_value())
                {
                    continue;
                }
                const auto kind = component->gallery_media->classify_extension(extension.c_str());
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
        const std::wstring& path,
        const std::wstring& language_tag)
    {
        if (component.api.query_loading_text == nullptr)
        {
            return {};
        }

        ComponentLoadingTextResult result;
        if (!component.api.query_loading_text(
                path.c_str(),
                language_tag.c_str(),
                &result))
        {
            return {};
        }
        const auto length = wcsnlen_s(result.text, std::size(result.text));
        return length == std::size(result.text)
            ? std::wstring{}
            : std::wstring(result.text, length);
    }

    std::wstring query_refinement_text(
        const RefinementSession& session,
        const std::wstring& language_tag)
    {
        ComponentLoadingTextResult result;
        if (!session.api.query_refinement_text(
                session.token,
                language_tag.c_str(),
                &result))
        {
            return {};
        }
        const auto length = wcsnlen_s(result.text, std::size(result.text));
        return length == std::size(result.text)
            ? std::wstring{}
            : std::wstring(result.text, length);
    }

    glance::app::ComponentPreviewResult materialize_preview(
        const std::shared_ptr<LoadedComponent>& component,
        const PreparedPreview& preview)
    {
        glance::app::ComponentPreviewResult result;
        result.status = glance::contracts::components::PrepareStatus::success;
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
        result.format = preview.format;
        result.output_path = output.wstring();
        result.lease = std::make_shared<PreviewLease>(
            component,
            preview.lease_token);
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
                glance::contracts::components::maximum_web_resource_mappings)
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
                copied.values.push_back(glance::app::FileDirectoryValue{
                    .kind = value.kind,
                    .unsigned_value = value.unsigned_value,
                    .ratio_value = value.ratio_value,
                    .text = value.text == nullptr ? L"" : value.text });
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

    ComponentLoadingMessage component_loading_text(
        const std::wstring& path,
        std::wstring_view language_tag) noexcept
    {
        try
        {
            initialize_components();
            const std::wstring language(language_tag);
            for (const auto& component : candidates_for_path(path))
            {
                if (component->api.can_preview(path.c_str()))
                {
                    return ComponentLoadingMessage{
                        .component_found = true,
                        .text = query_loading_text(*component, path, language) };
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
        const ComponentLoadingTextCallback& loading_callback) noexcept
    {
        ComponentPreviewResult result;
        try
        {
            initialize_components();
            const std::wstring language(language_tag);
            for (const auto& component : candidates_for_path(path))
            {
                if (!component->api.can_preview(path.c_str()))
                {
                    continue;
                }
                if (loading_callback)
                {
                    try
                    {
                        loading_callback(query_loading_text(*component, path, language));
                    }
                    catch (...)
                    {
                    }
                }

                PreparedPreview preview;
                options.size = sizeof(options);
                result.status = component->configurable_preview.has_value()
                    ? component->configurable_preview->prepare_preview(
                        path.c_str(),
                        language.c_str(),
                        &options,
                        &preview)
                    : component->api.prepare_preview(
                        path.c_str(),
                        language.c_str(),
                        &preview);
                result.error_detail = preview.error_detail;
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
                if (preview.lease_token != 0 &&
                    component->preview_notice.has_value())
                {
                    ComponentLoadingTextResult notice;
                    if (component->preview_notice->query_preview_notice(
                            preview.lease_token,
                            language.c_str(),
                            &notice))
                    {
                        const auto length =
                            wcsnlen_s(notice.text, std::size(notice.text));
                        if (length != std::size(notice.text))
                        {
                            result.notice.assign(notice.text, length);
                        }
                    }
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
                    session->options = options;
                    session->kind = preview.kind;
                    session->format = preview.format;
                    session->token = preview.lease_token;
                    result.refinement_text =
                        query_refinement_text(*session, language);
                    result.refinement = std::move(session);
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
            const std::wstring language(language_tag);
            result.status = session->api.prepare_refined_preview(
                session->token,
                language.c_str(),
                &session->options,
                &preview);
            result.error_detail = preview.error_detail;
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
            const std::wstring language(language_tag);
            const std::wstring password_value(password);
            const auto status = session->api.open(
                session->token,
                language.c_str(),
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
                const auto label = bounded_string(field.label);
                const auto value = bounded_string(field.text);
                if (!id.has_value() || !valid_setting_id(*id) ||
                    !info_ids.insert(*id).second ||
                    !label.has_value() || label->empty() || !value.has_value() ||
                    !valid_file_directory_value_kind(field.kind))
                {
                    return FileDirectoryOpenStatus::failed;
                }
                descriptor.info_fields.push_back(FileDirectoryInfoField{
                    .id = *id,
                    .label = *label,
                    .value = FileDirectoryValue{
                        .kind = field.kind,
                        .unsigned_value = field.unsigned_value,
                        .ratio_value = field.ratio_value,
                        .text = *value } });
            }
            descriptor.columns.reserve(native.column_count);
            std::unordered_set<std::wstring> column_ids;
            for (std::uint32_t index = 0; index < native.column_count; ++index)
            {
                const auto& column = native.columns[index];
                const auto id = bounded_string(column.id);
                const auto title = bounded_string(column.title);
                if (!id.has_value() || !valid_setting_id(*id) ||
                    !column_ids.insert(*id).second ||
                    !title.has_value() || title->empty() ||
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
                    .title = *title,
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
            FileDirectoryEntryCollector collector{ .entries = &result.entries };
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
        for (const auto& component : components)
        {
            try
            {
                glance::contracts::components::ComponentStatusResult status;
                if (!component->api.query_status(
                        std::wstring(language_tag).c_str(),
                        &status) ||
                    status.display_name[0] == L'\0')
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
                std::wstring detail = status.detail;
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
                    .display_name = status.display_name,
                    .detail = std::move(detail),
                    .state = state });
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

    std::vector<ComponentSetting> component_settings(
        std::wstring_view language_tag) noexcept
    {
        std::vector<std::shared_ptr<LoadedComponent>> components;
        {
            initialize_components();
            std::scoped_lock lock(registry_mutex);
            components = registered_components;
        }

        std::vector<ComponentSetting> settings;
        const std::wstring language(language_tag);
        for (const auto& component : components)
        {
            if (!component->active || !component->settings_contribution.has_value())
            {
                continue;
            }
            try
            {
                std::uint32_t count{};
                if (!component->settings_contribution->enumerate_settings(
                        language.c_str(), nullptr, 0, &count) ||
                    count == 0 || count > 64)
                {
                    continue;
                }
                std::vector<glance::contracts::components::ComponentSettingDescriptor>
                    descriptors(count);
                std::uint32_t written = count;
                if (!component->settings_contribution->enumerate_settings(
                        language.c_str(), descriptors.data(), count, &written) ||
                    written != count)
                {
                    continue;
                }
                for (const auto& descriptor : descriptors)
                {
                    const auto setting_id = bounded_string(descriptor.setting_id);
                    const auto group_id = bounded_string(descriptor.group_id);
                    const auto group_title = bounded_string(descriptor.group_title);
                    const auto label = bounded_string(descriptor.label);
                    const auto description = bounded_string(descriptor.description);
                    if (descriptor.size < sizeof(descriptor) ||
                        !setting_id.has_value() || !valid_setting_id(*setting_id) ||
                        !group_id.has_value() || !valid_setting_id(*group_id) ||
                        !group_title.has_value() || group_title->empty() ||
                        !label.has_value() || label->empty() ||
                        !description.has_value() ||
                        (descriptor.kind !=
                             glance::contracts::components::ComponentSettingKind::toggle &&
                         descriptor.kind !=
                             glance::contracts::components::ComponentSettingKind::choice) ||
                        descriptor.option_count >
                            glance::contracts::components::maximum_setting_options ||
                        (descriptor.kind ==
                             glance::contracts::components::ComponentSettingKind::choice &&
                         descriptor.option_count == 0))
                    {
                        continue;
                    }
                    ComponentSetting setting{
                        .component_id = component->id,
                        .setting_id = std::move(*setting_id),
                        .page = descriptor.page,
                        .group_id = std::move(*group_id),
                        .group_title = std::move(*group_title),
                        .label = std::move(*label),
                        .description = std::move(*description),
                        .kind = descriptor.kind,
                        .default_value = descriptor.default_value,
                        .group_order = descriptor.group_order,
                        .setting_order = descriptor.setting_order };
                    for (std::uint32_t index = 0;
                         index < descriptor.option_count;
                         ++index)
                    {
                        const auto text = bounded_string(descriptor.options[index].text);
                        if (!text.has_value() || text->empty())
                        {
                            setting.options.clear();
                            break;
                        }
                        setting.options.push_back(ComponentSettingOption{
                            .value = descriptor.options[index].value,
                            .text = std::move(*text) });
                    }
                    if (descriptor.kind ==
                            glance::contracts::components::ComponentSettingKind::toggle ||
                        !setting.options.empty())
                    {
                        settings.push_back(std::move(setting));
                    }
                }
            }
            catch (...)
            {
            }
        }
        std::ranges::sort(settings, [](const auto& left, const auto& right) {
            return std::tie(
                       left.page,
                       left.group_order,
                       left.component_id,
                       left.group_id,
                       left.setting_order,
                       left.setting_id) <
                std::tie(
                       right.page,
                       right.group_order,
                       right.component_id,
                       right.group_id,
                       right.setting_order,
                       right.setting_id);
        });
        return settings;
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
            HKEY key{};
            if (RegOpenKeyExW(HKEY_CURRENT_USER, key_path.c_str(), 0, KEY_QUERY_VALUE, &key) ==
                ERROR_SUCCESS)
            {
                ULONGLONG value{};
                DWORD type{};
                DWORD size = sizeof(value);
                const auto status = RegQueryValueExW(
                    key,
                    std::wstring(setting_id).c_str(),
                    nullptr,
                    &type,
                    reinterpret_cast<BYTE*>(&value),
                    &size);
                RegCloseKey(key);
                if (status == ERROR_SUCCESS && type == REG_QWORD && size == sizeof(value))
                {
                    return static_cast<std::int64_t>(value);
                }
            }

            if (component_id == L"pdf" && setting_id == L"render-dimension")
            {
                HKEY legacy_key{};
                if (RegOpenKeyExW(
                        HKEY_CURRENT_USER,
                        L"Software\\Glance\\MediaPreview",
                        0,
                        KEY_QUERY_VALUE,
                        &legacy_key) == ERROR_SUCCESS)
                {
                    DWORD value{};
                    DWORD type{};
                    DWORD size = sizeof(value);
                    const auto status = RegQueryValueExW(
                        legacy_key,
                        L"RichDocumentRenderDimension",
                        nullptr,
                        &type,
                        reinterpret_cast<BYTE*>(&value),
                        &size);
                    RegCloseKey(legacy_key);
                    if (status == ERROR_SUCCESS && type == REG_DWORD && size == sizeof(value))
                    {
                        save_component_setting_value(component_id, setting_id, value);
                        return value;
                    }
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
            HKEY key{};
            if (RegCreateKeyExW(
                    HKEY_CURRENT_USER,
                    key_path.c_str(),
                    0,
                    nullptr,
                    0,
                    KEY_SET_VALUE,
                    nullptr,
                    &key,
                    nullptr) != ERROR_SUCCESS)
            {
                return;
            }
            const auto stored = static_cast<ULONGLONG>(value);
            static_cast<void>(RegSetValueExW(
                key,
                std::wstring(setting_id).c_str(),
                0,
                REG_QWORD,
                reinterpret_cast<const BYTE*>(&stored),
                sizeof(stored)));
            RegCloseKey(key);
        }
        catch (...)
        {
        }
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

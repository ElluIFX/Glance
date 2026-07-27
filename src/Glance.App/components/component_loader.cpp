#include "pch.h"
#include "component_loader.h"

#include "glance/contracts/diagnostics.h"
#include "../../version.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

namespace
{
    using glance::contracts::components::ComponentApi;
    using glance::contracts::components::ComponentLoadingTextResult;
    using glance::contracts::components::ComponentRegistration;
    using glance::contracts::components::GetApiFunction;
    using glance::contracts::components::HealthSeverity;
    using glance::contracts::components::PreparedPreview;
    using glance::contracts::components::PreviewContentFormat;
    using glance::contracts::components::PreviewContentKind;

    struct ComponentManifest
    {
        std::wstring id;
        std::wstring entry_point;
    };

    struct LoadedComponent
    {
        std::wstring id;
        HMODULE module{};
        ComponentApi api;
        ComponentRegistration registration;
        std::vector<std::wstring> extensions;

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

    std::once_flag initialization_flag;
    std::mutex registry_mutex;
    std::vector<std::shared_ptr<LoadedComponent>> registered_components;
    std::unordered_map<
        std::wstring,
        std::vector<std::shared_ptr<LoadedComponent>>> extension_index;

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
        default:
            return false;
        }
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
            if (object.GetNamedNumber(L"schema_version") != 2.0)
            {
                return false;
            }
            manifest.id = object.GetNamedString(L"id");
            manifest.entry_point = object.GetNamedString(L"entry_point");
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
            auto& extensions = *static_cast<std::vector<std::wstring>*>(context);
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

    std::shared_ptr<LoadedComponent> load_component(
        const std::filesystem::path& directory,
        const ComponentManifest& manifest)
    {
        auto component = std::make_shared<LoadedComponent>();
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

        glance::contracts::components::ComponentRegistrar registrar{
            .context = &component->extensions,
            .register_extension = register_extension };
        if (!component->api.initialize(&registrar, &component->registration) ||
            component->registration.size < sizeof(ComponentRegistration) ||
            manifest.id != component->registration.component_id ||
            std::wstring_view(component->registration.target_app_version) !=
                GLANCE_VERSION_WSTRING ||
            component->extensions.empty() ||
            !valid_content_pair(
                component->registration.preferred_kind,
                component->registration.preferred_format))
        {
            return {};
        }
        component->id = manifest.id;
        std::ranges::sort(component->extensions);
        return component;
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

        std::unordered_map<
            std::wstring,
            std::vector<std::shared_ptr<LoadedComponent>>> index;
        for (const auto& component : loaded)
        {
            for (const auto& extension : component->extensions)
            {
                index[extension].push_back(component);
            }
        }

        std::scoped_lock lock(registry_mutex);
        registered_components = std::move(loaded);
        extension_index = std::move(index);
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
                result.status = component->api.prepare_preview(
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
                if (!valid_content_pair(preview.kind, preview.format) ||
                    preview.path[0] == L'\0')
                {
                    component->api.release_preview(preview.lease_token);
                    result.status =
                        glance::contracts::components::PrepareStatus::failed;
                    return result;
                }

                const std::filesystem::path output(preview.path);
                std::error_code error;
                if (!output.is_absolute() ||
                    !std::filesystem::is_regular_file(output, error))
                {
                    component->api.release_preview(preview.lease_token);
                    result.status =
                        glance::contracts::components::PrepareStatus::failed;
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
        }
        catch (...)
        {
            result.status = glance::contracts::components::PrepareStatus::failed;
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
                statuses.push_back(ComponentStatus{
                    .id = component->id,
                    .display_name = status.display_name,
                    .detail = status.detail,
                    .state = state });
            }
            catch (...)
            {
            }
        }
        return statuses;
    }

    void shutdown_components() noexcept
    {
        std::vector<std::shared_ptr<LoadedComponent>> components;
        {
            std::scoped_lock lock(registry_mutex);
            extension_index.clear();
            components = std::move(registered_components);
        }
        components.clear();
    }
}

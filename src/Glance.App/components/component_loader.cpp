#include "pch.h"
#include "component_loader.h"

#include "component_catalog.h"
#include "localization.h"
#include "../../version.h"

#include <array>
#include <filesystem>
#include <mutex>
#include <unordered_map>

namespace
{
    using glance::app::ComponentState;
    using glance::app::SupportedComponent;
    using glance::contracts::components::ComponentApi;
    using glance::contracts::components::GetApiFunction;
    using glance::contracts::components::HealthSeverity;

    struct LoadedComponent
    {
        bool attempted{};
        HMODULE module{};
        ComponentApi api;
        ComponentState state{ ComponentState::not_installed };
        glance::contracts::components::HealthResult health;
    };

    std::mutex component_mutex;
    std::unordered_map<std::wstring, LoadedComponent> loaded_components;

    std::filesystem::path executable_directory()
    {
        std::wstring path(32768, L'\0');
        const DWORD length =
            GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

    std::filesystem::path locate_component(const SupportedComponent& descriptor)
    {
        const auto application_directory =
            glance::app::application_component_root() / descriptor.id;
        std::error_code error;
        if (std::filesystem::exists(application_directory, error))
        {
            return application_directory;
        }
        return {};
    }

    LoadedComponent& load_component_locked(const SupportedComponent& descriptor)
    {
        auto& loaded = loaded_components[std::wstring(descriptor.id)];
        if (loaded.attempted)
        {
            return loaded;
        }
        loaded.attempted = true;

        const auto directory = locate_component(descriptor);
        if (directory.empty())
        {
            return loaded;
        }

        const auto entry_path = directory / descriptor.entry_point;
        loaded.module = LoadLibraryExW(
            entry_path.c_str(),
            nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (loaded.module == nullptr)
        {
            loaded.state = ComponentState::damaged;
            return loaded;
        }

        const auto get_api = reinterpret_cast<GetApiFunction>(
            GetProcAddress(
                loaded.module,
                glance::contracts::components::get_api_export));
        if (get_api == nullptr ||
            !get_api(glance::contracts::components::abi_version, &loaded.api) ||
            descriptor.id != loaded.api.component_id ||
            std::wstring_view(loaded.api.target_app_version) != GLANCE_VERSION_WSTRING ||
            descriptor.output_kind != loaded.api.output_kind)
        {
            loaded.state = ComponentState::incompatible;
            return loaded;
        }

        loaded.state = ComponentState::healthy;
        return loaded;
    }

    void query_health_locked(LoadedComponent& loaded, std::wstring_view language)
    {
        if (loaded.state == ComponentState::not_installed ||
            loaded.state == ComponentState::incompatible ||
            loaded.state == ComponentState::damaged)
        {
            return;
        }

        loaded.health = {};
        if (loaded.api.query_health == nullptr ||
            !loaded.api.query_health(std::wstring(language).c_str(), &loaded.health))
        {
            loaded.state = ComponentState::damaged;
        }
        else if (loaded.health.severity == HealthSeverity::warning)
        {
            loaded.state = ComponentState::warning;
        }
        else if (loaded.health.severity == HealthSeverity::error)
        {
            loaded.state = ComponentState::error;
        }
        else
        {
            loaded.state = ComponentState::healthy;
        }
    }
}

namespace glance::app
{
    std::filesystem::path application_component_root()
    {
        return executable_directory() / L"components";
    }

    bool component_can_preview(std::wstring_view path) noexcept
    {
        try
        {
            const auto* descriptor =
                find_component_for_extension(std::filesystem::path(path).extension().wstring());
            if (descriptor == nullptr)
            {
                return false;
            }
            std::scoped_lock lock(component_mutex);
            auto& loaded = load_component_locked(*descriptor);
            query_health_locked(loaded, current_ui_language());
            return (loaded.state == ComponentState::healthy ||
                    loaded.state == ComponentState::warning) &&
                loaded.api.can_preview != nullptr &&
                loaded.api.can_preview(std::wstring(path).c_str());
        }
        catch (...)
        {
            return false;
        }
    }

    ComponentPreviewResult prepare_component_preview(const std::wstring& path) noexcept
    {
        ComponentPreviewResult result;
        try
        {
            const auto* descriptor =
                find_component_for_extension(std::filesystem::path(path).extension().wstring());
            if (descriptor == nullptr)
            {
                return result;
            }
            std::scoped_lock lock(component_mutex);
            auto& loaded = load_component_locked(*descriptor);
            query_health_locked(loaded, current_ui_language());
            if ((loaded.state != ComponentState::healthy &&
                 loaded.state != ComponentState::warning) ||
                loaded.api.prepare_preview == nullptr)
            {
                return result;
            }

            std::array<wchar_t, 32768> output{};
            result.status = loaded.api.prepare_preview(
                path.c_str(),
                output.data(),
                static_cast<std::uint32_t>(output.size()));
            if (result.status == glance::contracts::components::PrepareStatus::success)
            {
                result.output_path = output.data();
            }
        }
        catch (...)
        {
            result.status = glance::contracts::components::PrepareStatus::failed;
        }
        return result;
    }

    std::vector<ComponentStatus> component_statuses() noexcept
    {
        std::vector<ComponentStatus> statuses;
        try
        {
            std::scoped_lock lock(component_mutex);
            for (const auto& descriptor : supported_components())
            {
                auto& loaded = load_component_locked(descriptor);
                query_health_locked(loaded, current_ui_language());
                statuses.push_back(ComponentStatus{
                    .id = std::wstring(descriptor.id),
                    .state = loaded.state,
                    .health = loaded.health });
            }
        }
        catch (...)
        {
        }
        return statuses;
    }

    void shutdown_components() noexcept
    {
        std::scoped_lock lock(component_mutex);
        for (auto& [id, loaded] : loaded_components)
        {
            static_cast<void>(id);
            if (loaded.api.shutdown != nullptr)
            {
                loaded.api.shutdown();
            }
            if (loaded.module != nullptr)
            {
                FreeLibrary(loaded.module);
            }
        }
        loaded_components.clear();
    }
}

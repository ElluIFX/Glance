#pragma once

#include "glance/contracts/component_api.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace glance::app
{
    enum class ComponentState
    {
        not_installed,
        healthy,
        warning,
        error,
        incompatible,
        damaged,
    };

    struct ComponentStatus
    {
        std::wstring id;
        ComponentState state{ ComponentState::not_installed };
        glance::contracts::components::HealthResult health;
    };

    struct ComponentPreviewResult
    {
        glance::contracts::components::PrepareStatus status{
            glance::contracts::components::PrepareStatus::unavailable };
        std::wstring output_path;
    };

    [[nodiscard]] std::filesystem::path application_component_root();
    [[nodiscard]] bool component_can_preview(std::wstring_view path) noexcept;
    [[nodiscard]] ComponentPreviewResult prepare_component_preview(
        const std::wstring& path) noexcept;
    [[nodiscard]] std::vector<ComponentStatus> component_statuses() noexcept;
    void shutdown_components() noexcept;
}

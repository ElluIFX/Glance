#pragma once

#include "glance/contracts/component_api.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace glance::app
{
    using ComponentLoadingTextCallback = std::function<void(std::wstring)>;

    enum class ComponentState
    {
        healthy,
        warning,
        error,
    };

    struct ComponentStatus
    {
        std::wstring id;
        std::wstring display_name;
        std::wstring detail;
        ComponentState state{ ComponentState::error };
    };

    struct ComponentLoadingMessage
    {
        bool component_found{};
        std::wstring text;
    };

    struct ComponentPreviewResult
    {
        glance::contracts::components::PrepareStatus status{
            glance::contracts::components::PrepareStatus::unavailable };
        glance::contracts::components::PreviewContentKind kind{
            glance::contracts::components::PreviewContentKind::none };
        glance::contracts::components::PreviewContentFormat format{
            glance::contracts::components::PreviewContentFormat::none };
        std::wstring output_path;
        std::wstring error_detail;
        std::shared_ptr<void> lease;
        std::shared_ptr<void> refinement;
        std::wstring refinement_text;
        std::wstring notice;
    };

    [[nodiscard]] std::filesystem::path application_component_root();
    void initialize_components() noexcept;
    [[nodiscard]] bool component_has_extension(std::wstring_view extension) noexcept;
    [[nodiscard]] ComponentLoadingMessage component_loading_text(
        const std::wstring& path,
        std::wstring_view language_tag) noexcept;
    [[nodiscard]] ComponentPreviewResult prepare_component_preview(
        const std::wstring& path,
        std::wstring_view language_tag,
        glance::contracts::components::PreviewPreparationOptions options,
        const ComponentLoadingTextCallback& loading_callback) noexcept;
    [[nodiscard]] ComponentPreviewResult refine_component_preview(
        const std::shared_ptr<void>& refinement,
        std::wstring_view language_tag) noexcept;
    [[nodiscard]] std::vector<ComponentStatus> component_statuses(
        std::wstring_view language_tag) noexcept;
    void shutdown_components() noexcept;
}

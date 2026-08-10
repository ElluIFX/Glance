#pragma once

#include "glance/contracts/component_api.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
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

    struct ComponentWebResourceMapping
    {
        std::wstring host_name;
        std::wstring folder_path;
        glance::contracts::components::WebResourceAccessKind access_kind{
            glance::contracts::components::WebResourceAccessKind::deny_cors };
    };

    struct ComponentWebPreview
    {
        std::wstring navigation_uri;
        std::vector<ComponentWebResourceMapping> mappings;
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
        std::shared_ptr<ComponentWebPreview> web_preview;
        std::shared_ptr<void> file_directory;
        std::wstring refinement_text;
        std::wstring notice;
    };

    struct FileDirectoryValue
    {
        glance::contracts::components::FileDirectoryValueKind kind{
            glance::contracts::components::FileDirectoryValueKind::none };
        std::uint64_t unsigned_value{};
        double ratio_value{};
        std::wstring text;
    };

    struct FileDirectoryInfoField
    {
        std::wstring id;
        std::wstring label;
        FileDirectoryValue value;
    };

    struct FileDirectoryColumn
    {
        std::wstring id;
        std::wstring title;
        glance::contracts::components::FileDirectoryValueKind kind{
            glance::contracts::components::FileDirectoryValueKind::text };
        glance::contracts::components::FileDirectoryAlignment alignment{
            glance::contracts::components::FileDirectoryAlignment::left };
        std::uint32_t width{};
        bool sortable{};
    };

    struct FileDirectoryDescriptor
    {
        glance::contracts::components::FileDirectoryPresentation presentation{
            glance::contracts::components::FileDirectoryPresentation::tree };
        std::vector<FileDirectoryInfoField> info_fields;
        std::vector<FileDirectoryColumn> columns;
        bool truncated{};
        bool depth_limited{};
    };

    struct FileDirectoryEntry
    {
        std::uint64_t node_id{};
        bool is_folder{};
        bool has_children{};
        std::wstring name;
        std::wstring icon_key;
        std::vector<FileDirectoryValue> values;
    };

    struct FileDirectoryPage
    {
        std::vector<FileDirectoryEntry> entries;
        std::uint32_t total{};
        bool failed{};
    };

    struct PagedDocumentRendererRegistration
    {
        std::wstring host_path;
        std::shared_ptr<void> lease;
    };

    struct ComponentSettingOption
    {
        std::int64_t value{};
        std::wstring text;
    };

    struct ComponentSetting
    {
        std::wstring component_id;
        std::wstring setting_id;
        glance::contracts::components::ComponentSettingPage page{
            glance::contracts::components::ComponentSettingPage::document_preview };
        std::wstring group_id;
        std::wstring group_title;
        std::wstring label;
        std::wstring description;
        glance::contracts::components::ComponentSettingKind kind{
            glance::contracts::components::ComponentSettingKind::choice };
        std::int64_t default_value{};
        std::uint32_t group_order{};
        std::uint32_t setting_order{};
        std::vector<ComponentSettingOption> options;
    };

    [[nodiscard]] std::filesystem::path application_component_root();
    void initialize_components() noexcept;
    [[nodiscard]] bool component_has_extension(std::wstring_view extension) noexcept;
    [[nodiscard]] glance::contracts::components::GalleryMediaKind
        component_gallery_media_kind(std::wstring_view extension) noexcept;
    [[nodiscard]] std::vector<std::wstring> component_gallery_extensions(
        glance::contracts::components::GalleryMediaKind kind) noexcept;
    [[nodiscard]] ComponentLoadingMessage component_loading_text(
        const std::wstring& path,
        std::wstring_view language_tag) noexcept;
    [[nodiscard]] ComponentPreviewResult prepare_component_preview(
        const std::wstring& path,
        std::wstring_view language_tag,
        glance::contracts::components::PreviewPreparationOptions options,
        glance::contracts::components::PreviewColorScheme color_scheme,
        const ComponentLoadingTextCallback& loading_callback) noexcept;
    [[nodiscard]] ComponentPreviewResult refine_component_preview(
        const std::shared_ptr<void>& refinement,
        std::wstring_view language_tag) noexcept;
    [[nodiscard]] glance::contracts::components::FileDirectoryOpenStatus
        open_component_file_directory(
            const std::shared_ptr<void>& session,
            std::wstring_view language_tag,
            std::wstring_view password,
            FileDirectoryDescriptor& descriptor) noexcept;
    [[nodiscard]] FileDirectoryPage enumerate_component_file_directory(
        const std::shared_ptr<void>& session,
        std::uint64_t parent_node_id,
        std::uint32_t offset,
        std::uint32_t limit) noexcept;
    [[nodiscard]] std::vector<ComponentStatus> component_statuses(
        std::wstring_view language_tag) noexcept;
    [[nodiscard]] std::optional<PagedDocumentRendererRegistration>
        paged_document_renderer() noexcept;
    [[nodiscard]] std::vector<ComponentSetting> component_settings(
        std::wstring_view language_tag) noexcept;
    [[nodiscard]] std::int64_t component_setting_value(
        std::wstring_view component_id,
        std::wstring_view setting_id,
        std::int64_t default_value) noexcept;
    void save_component_setting_value(
        std::wstring_view component_id,
        std::wstring_view setting_id,
        std::int64_t value) noexcept;
    void shutdown_components() noexcept;
}

#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace glance::contracts::components
{
    inline constexpr std::uint32_t abi_version = 6;
    inline constexpr char get_api_export[] = "GlanceComponentGetApi";
    inline constexpr std::size_t component_id_capacity = 64;
    inline constexpr std::size_t target_app_version_capacity = 32;
    inline constexpr std::size_t display_name_capacity = 128;
    inline constexpr std::size_t status_detail_capacity = 256;
    inline constexpr std::size_t loading_text_capacity = 256;
    inline constexpr std::size_t preview_path_capacity = 32768;
    inline constexpr std::size_t preview_error_capacity = 256;
    inline constexpr std::uint32_t configurable_preview_api_version = 1;
    inline constexpr std::uint32_t progressive_preview_api_version = 1;
    inline constexpr std::uint32_t preview_notice_api_version = 1;
    inline constexpr std::uint32_t web_preview_api_version = 1;
    inline constexpr std::uint32_t paged_document_renderer_api_version = 1;
    inline constexpr std::uint32_t settings_contribution_api_version = 1;
    inline constexpr std::uint32_t file_directory_preview_api_version = 1;
    inline constexpr std::uint32_t gallery_media_api_version = 1;
    inline constexpr std::uint32_t image_metadata_api_version = 1;
    inline constexpr std::uint32_t hover_info_layer_api_version = 1;
    inline constexpr std::uint32_t status_bar_shortcut_api_version = 1;
    inline constexpr std::uint32_t status_bar_shortcut_data_api_version = 1;
    inline constexpr std::uint32_t component_management_action_api_version = 1;
    inline constexpr std::size_t web_resource_host_capacity = 64;
    inline constexpr std::size_t maximum_web_resource_mappings = 4;
    inline constexpr std::size_t renderer_host_capacity = 260;
    inline constexpr std::size_t setting_id_capacity = 64;
    inline constexpr std::size_t setting_group_id_capacity = 64;
    inline constexpr std::size_t setting_text_capacity = 256;
    inline constexpr std::size_t setting_option_text_capacity = 64;
    inline constexpr std::size_t maximum_setting_options = 8;
    inline constexpr std::size_t file_directory_id_capacity = 64;
    inline constexpr std::size_t file_directory_text_capacity = 256;
    inline constexpr std::size_t maximum_file_directory_info_fields = 8;
    inline constexpr std::size_t maximum_file_directory_columns = 8;
    inline constexpr std::size_t contribution_id_capacity = 64;
    inline constexpr std::size_t contribution_text_capacity = 256;
    inline constexpr std::size_t maximum_status_bar_shortcuts = 8;
    inline constexpr std::size_t maximum_component_management_actions = 4;
    inline constexpr std::size_t download_url_capacity = 2048;
    inline constexpr std::size_t download_file_name_capacity = 260;
    inline constexpr std::size_t sha256_capacity = 65;
    inline constexpr std::size_t image_metadata_property_capacity = 128;
    inline constexpr std::size_t image_metadata_text_capacity = 512;
    inline constexpr GUID configurable_preview_api_id{
        0x950742e7,
        0x0af2,
        0x49a1,
        { 0xa0, 0xb3, 0xe5, 0x76, 0xe4, 0x23, 0x24, 0xc5 } };
    inline constexpr GUID progressive_preview_api_id{
        0xf9037d7d,
        0xe9e8,
        0x4065,
        { 0xa9, 0x47, 0x20, 0xe2, 0xae, 0xf2, 0xb8, 0x57 } };
    inline constexpr GUID preview_notice_api_id{
        0xa0b393b6,
        0x0485,
        0x4a6c,
        { 0x9b, 0x96, 0x46, 0x27, 0xf5, 0x2c, 0xdd, 0x14 } };
    inline constexpr GUID web_preview_api_id{
        0x934213d4,
        0x840d,
        0x49f5,
        { 0xa7, 0xbc, 0x77, 0xe0, 0x2d, 0x49, 0xd6, 0x28 } };
    inline constexpr GUID paged_document_renderer_api_id{
        0x3ce11f55,
        0x620e,
        0x4fc2,
        { 0x89, 0x41, 0x19, 0xd2, 0x39, 0x29, 0xe2, 0x66 } };
    inline constexpr GUID settings_contribution_api_id{
        0xd1ef7371,
        0x3b06,
        0x46a3,
        { 0xa1, 0xc0, 0xae, 0x40, 0x78, 0x3f, 0xf2, 0xdc } };
    inline constexpr GUID file_directory_preview_api_id{
        0xd350908d,
        0xc8dc,
        0x462f,
        { 0x95, 0x8a, 0xe1, 0x1f, 0xc7, 0xaf, 0xc1, 0xda } };
    inline constexpr GUID gallery_media_api_id{
        0x80dbd46d,
        0xa62c,
        0x4b43,
        { 0x9c, 0xcd, 0x08, 0x26, 0x09, 0xaa, 0xd2, 0xd7 } };
    inline constexpr GUID image_metadata_api_id{
        0xb9bfade4,
        0xdc6c,
        0x4dcc,
        { 0x81, 0x4b, 0x67, 0xe8, 0x3e, 0xf4, 0x36, 0x1b } };
    inline constexpr GUID hover_info_layer_api_id{
        0x422e5648,
        0x8724,
        0x49a5,
        { 0x8f, 0xf4, 0xb3, 0xc4, 0xc1, 0x0e, 0x9f, 0x19 } };
    inline constexpr GUID status_bar_shortcut_api_id{
        0xa4b14761,
        0x38fc,
        0x43c3,
        { 0x89, 0xd5, 0x24, 0x71, 0x19, 0xb1, 0xdf, 0x79 } };
    inline constexpr GUID status_bar_shortcut_data_api_id{
        0xd32a7c73,
        0xc6e6,
        0x472b,
        { 0xa7, 0xc0, 0x69, 0x95, 0xfd, 0x74, 0x68, 0xd1 } };
    inline constexpr GUID component_management_action_api_id{
        0xa41f0c2d,
        0x3002,
        0x46f6,
        { 0x8b, 0xce, 0xb4, 0x51, 0x54, 0x4b, 0xc4, 0x62 } };

    enum class PreviewContentKind : std::uint32_t
    {
        none = 0,
        text = 1,
        image = 2,
        media = 3,
        document = 4,
        web = 5,
        directory = 6,
    };

    enum class PreviewContentFormat : std::uint32_t
    {
        none = 0,
        plain_text = 1,
        markdown = 2,
        image_file = 3,
        media_file = 4,
        pdf = 5,
        html = 6,
        file_directory = 7,
    };

    enum class PrepareStatus : std::uint32_t
    {
        success = 0,
        unavailable = 1,
        failed = 2,
        cancelled = 3,
    };

    enum class GalleryMediaKind : std::uint32_t
    {
        none = 0,
        image = 1,
        video = 2,
        audio = 3,
    };

    enum class ImageMetadataValueKind : std::uint32_t
    {
        text = 0,
        unsigned_integer = 1,
        floating_point = 2,
        timestamp = 3,
    };

    enum class HealthSeverity : std::uint32_t
    {
        healthy = 0,
        warning = 1,
        error = 2,
    };

    enum class PreviewColorScheme : std::uint32_t
    {
        light = 0,
        dark = 1,
    };

    enum class WebResourceAccessKind : std::uint32_t
    {
        deny_cors = 0,
        allow = 1,
    };

    enum class ComponentSettingPage : std::uint32_t
    {
        document_preview = 1,
    };

    enum class ComponentSettingKind : std::uint32_t
    {
        toggle = 1,
        choice = 2,
    };

    enum class FileDirectoryPresentation : std::uint32_t
    {
        list = 0,
        tree = 1,
    };

    enum class FileDirectoryValueKind : std::uint32_t
    {
        none = 0,
        text = 1,
        unsigned_integer = 2,
        bytes = 3,
        timestamp = 4,
        ratio = 5,
    };

    enum class FileDirectoryAlignment : std::uint32_t
    {
        left = 0,
        right = 1,
    };

    enum class FileDirectoryOpenStatus : std::uint32_t
    {
        ready = 0,
        password_required = 1,
        invalid_password = 2,
        failed = 3,
        cancelled = 4,
    };

    enum class StatusBarShortcutState : std::uint32_t
    {
        hidden = 0,
        ready = 1,
        setup_required = 2,
    };

    enum class StatusBarShortcutActivation : std::uint32_t
    {
        none = 0,
        toggle_hover_info = 1,
        request_component_action = 2,
    };

    using RegisterExtensionFunction = BOOL(WINAPI*)(
        void* context,
        const wchar_t* extension) noexcept;
    using RegisterRendererFunction = BOOL(WINAPI*)(
        void* context,
        PreviewContentKind kind,
        PreviewContentFormat format,
        const GUID* interface_id,
        std::uint32_t interface_version) noexcept;

    struct ComponentRegistrar
    {
        std::uint32_t size{ sizeof(ComponentRegistrar) };
        void* context{};
        RegisterExtensionFunction register_extension{};
        RegisterRendererFunction register_renderer{};
    };

    struct ComponentRegistration
    {
        std::uint32_t size{ sizeof(ComponentRegistration) };
        wchar_t component_id[component_id_capacity]{};
        wchar_t target_app_version[target_app_version_capacity]{};
        PreviewContentKind preferred_kind{ PreviewContentKind::none };
        PreviewContentFormat preferred_format{ PreviewContentFormat::none };
    };

    struct ComponentStatusResult
    {
        std::uint32_t size{ sizeof(ComponentStatusResult) };
        HealthSeverity severity{ HealthSeverity::error };
        std::uint32_t code{};
        std::uint64_t capability_mask{};
        wchar_t display_name[display_name_capacity]{};
        wchar_t detail[status_detail_capacity]{};
    };

    struct ComponentLoadingTextResult
    {
        std::uint32_t size{ sizeof(ComponentLoadingTextResult) };
        wchar_t text[loading_text_capacity]{};
    };

    struct PreparedPreview
    {
        std::uint32_t size{ sizeof(PreparedPreview) };
        PreviewContentKind kind{ PreviewContentKind::none };
        PreviewContentFormat format{ PreviewContentFormat::none };
        std::uint64_t lease_token{};
        wchar_t path[preview_path_capacity]{};
        wchar_t error_detail[preview_error_capacity]{};
    };

    struct PreviewPreparationOptions
    {
        std::uint32_t size{ sizeof(PreviewPreparationOptions) };
        std::uint32_t maximum_dimension{ 4096 };
    };

    struct WebPreviewOptions
    {
        std::uint32_t size{ sizeof(WebPreviewOptions) };
        PreviewColorScheme color_scheme{ PreviewColorScheme::light };
    };

    struct WebResourceMapping
    {
        wchar_t host_name[web_resource_host_capacity]{};
        wchar_t folder_path[preview_path_capacity]{};
        WebResourceAccessKind access_kind{ WebResourceAccessKind::deny_cors };
    };

    struct WebPreviewDescriptor
    {
        std::uint32_t size{ sizeof(WebPreviewDescriptor) };
        wchar_t navigation_uri[preview_path_capacity]{};
        std::uint32_t mapping_count{};
        WebResourceMapping mappings[maximum_web_resource_mappings]{};
    };

    struct PagedDocumentHostDescriptor
    {
        std::uint32_t size{ sizeof(PagedDocumentHostDescriptor) };
        wchar_t host_executable[renderer_host_capacity]{};
    };

    struct ComponentSettingOption
    {
        std::int64_t value{};
        wchar_t text[setting_option_text_capacity]{};
    };

    struct ComponentSettingDescriptor
    {
        std::uint32_t size{ sizeof(ComponentSettingDescriptor) };
        wchar_t setting_id[setting_id_capacity]{};
        ComponentSettingPage page{ ComponentSettingPage::document_preview };
        wchar_t group_id[setting_group_id_capacity]{};
        wchar_t group_title[setting_text_capacity]{};
        wchar_t label[setting_text_capacity]{};
        wchar_t description[setting_text_capacity]{};
        ComponentSettingKind kind{ ComponentSettingKind::choice };
        std::int64_t default_value{};
        std::uint32_t group_order{};
        std::uint32_t setting_order{};
        std::uint32_t option_count{};
        ComponentSettingOption options[maximum_setting_options]{};
    };

    struct FileDirectoryValue
    {
        FileDirectoryValueKind kind{ FileDirectoryValueKind::none };
        std::uint64_t unsigned_value{};
        double ratio_value{};
        const wchar_t* text{};
    };

    struct FileDirectoryInfoField
    {
        wchar_t id[file_directory_id_capacity]{};
        wchar_t label[file_directory_text_capacity]{};
        FileDirectoryValueKind kind{ FileDirectoryValueKind::none };
        std::uint64_t unsigned_value{};
        double ratio_value{};
        wchar_t text[file_directory_text_capacity]{};
    };

    struct FileDirectoryColumnDescriptor
    {
        wchar_t id[file_directory_id_capacity]{};
        wchar_t title[file_directory_text_capacity]{};
        FileDirectoryValueKind kind{ FileDirectoryValueKind::text };
        FileDirectoryAlignment alignment{ FileDirectoryAlignment::left };
        std::uint32_t width{};
        BOOL sortable{};
    };

    struct FileDirectoryDescriptor
    {
        std::uint32_t size{ sizeof(FileDirectoryDescriptor) };
        FileDirectoryPresentation presentation{ FileDirectoryPresentation::tree };
        std::uint32_t info_field_count{};
        FileDirectoryInfoField info_fields[maximum_file_directory_info_fields]{};
        std::uint32_t column_count{};
        FileDirectoryColumnDescriptor columns[maximum_file_directory_columns]{};
        BOOL truncated{};
        BOOL depth_limited{};
    };

    struct FileDirectoryEntry
    {
        std::uint64_t node_id{};
        BOOL is_folder{};
        BOOL has_children{};
        const wchar_t* name{};
        const wchar_t* icon_key{};
        std::uint32_t value_count{};
        const FileDirectoryValue* values{};
    };

    struct HoverInfoTextSink
    {
        std::uint32_t size{ sizeof(HoverInfoTextSink) };
        void* context{};
        BOOL(WINAPI* append)(
            void* context,
            const wchar_t* text,
            std::uint32_t length) noexcept{};
        BOOL(WINAPI* is_cancelled)(void* context) noexcept{};
    };

    struct ImageMetadataEntry
    {
        std::uint32_t size{ sizeof(ImageMetadataEntry) };
        wchar_t canonical_name[image_metadata_property_capacity]{};
        ImageMetadataValueKind value_kind{ ImageMetadataValueKind::text };
        std::uint64_t unsigned_value{};
        double floating_point{};
        FILETIME timestamp{};
        wchar_t text[image_metadata_text_capacity]{};
    };

    struct ImageMetadataSink
    {
        std::uint32_t size{ sizeof(ImageMetadataSink) };
        void* context{};
        BOOL(WINAPI* append)(
            void* context,
            const ImageMetadataEntry* entry) noexcept{};
        BOOL(WINAPI* is_cancelled)(void* context) noexcept{};
    };

    struct StatusBarShortcutDescriptor
    {
        std::uint32_t size{ sizeof(StatusBarShortcutDescriptor) };
        wchar_t shortcut_id[contribution_id_capacity]{};
        PreviewContentKind target_kind{ PreviewContentKind::none };
        PreviewContentFormat target_format{ PreviewContentFormat::none };
        std::uint32_t order{};
        std::uint32_t fluent_icon_glyph{};
        wchar_t tooltip[contribution_text_capacity]{};
    };

    struct StatusBarShortcutActivationResult
    {
        std::uint32_t size{ sizeof(StatusBarShortcutActivationResult) };
        StatusBarShortcutActivation activation{ StatusBarShortcutActivation::none };
        BOOL checked{};
        wchar_t hover_info_id[contribution_id_capacity]{};
        wchar_t component_action_id[contribution_id_capacity]{};
        wchar_t loading_text[contribution_text_capacity]{};
    };

    struct ComponentManagementActionDescriptor
    {
        std::uint32_t size{ sizeof(ComponentManagementActionDescriptor) };
        wchar_t action_id[contribution_id_capacity]{};
        std::uint32_t order{};
        wchar_t button_text[contribution_text_capacity]{};
        wchar_t confirmation_title[contribution_text_capacity]{};
        wchar_t confirmation_message[contribution_text_capacity]{};
        wchar_t confirmation_button[contribution_text_capacity]{};
        wchar_t download_title[contribution_text_capacity]{};
        wchar_t download_message[contribution_text_capacity]{};
        wchar_t preparing_title[contribution_text_capacity]{};
        wchar_t preparing_message[contribution_text_capacity]{};
        wchar_t completed_title[contribution_text_capacity]{};
        wchar_t completed_message[contribution_text_capacity]{};
    };

    struct ComponentDownloadRequest
    {
        std::uint32_t size{ sizeof(ComponentDownloadRequest) };
        wchar_t url[download_url_capacity]{};
        wchar_t file_name[download_file_name_capacity]{};
        wchar_t sha256[sha256_capacity]{};
        std::uint64_t expected_size{};
    };

    struct ComponentManagementActionResult
    {
        std::uint32_t size{ sizeof(ComponentManagementActionResult) };
        BOOL succeeded{};
        wchar_t detail[contribution_text_capacity]{};
    };

    using AppendFileDirectoryEntryFunction = BOOL(WINAPI*)(
        void* context,
        const FileDirectoryEntry* entry) noexcept;

    struct FileDirectoryEntrySink
    {
        std::uint32_t size{ sizeof(FileDirectoryEntrySink) };
        void* context{};
        AppendFileDirectoryEntryFunction append{};
    };

    using InitializeFunction = BOOL(WINAPI*)(
        const ComponentRegistrar* registrar,
        ComponentRegistration* registration) noexcept;
    using QueryStatusFunction = BOOL(WINAPI*)(
        const wchar_t* language_tag,
        ComponentStatusResult* result) noexcept;
    using QueryLoadingTextFunction = BOOL(WINAPI*)(
        const wchar_t* path,
        const wchar_t* language_tag,
        ComponentLoadingTextResult* result) noexcept;
    using CanPreviewFunction = BOOL(WINAPI*)(const wchar_t* path) noexcept;
    using PreparePreviewFunction = PrepareStatus(WINAPI*)(
        const wchar_t* path,
        const wchar_t* language_tag,
        PreparedPreview* preview) noexcept;
    using ReleasePreviewFunction = void(WINAPI*)(std::uint64_t lease_token) noexcept;
    using PreparePreviewWithOptionsFunction = PrepareStatus(WINAPI*)(
        const wchar_t* path,
        const wchar_t* language_tag,
        const PreviewPreparationOptions* options,
        PreparedPreview* preview) noexcept;
    using CanRefinePreviewFunction = BOOL(WINAPI*)(
        std::uint64_t lease_token) noexcept;
    using QueryRefinementTextFunction = BOOL(WINAPI*)(
        std::uint64_t lease_token,
        const wchar_t* language_tag,
        ComponentLoadingTextResult* result) noexcept;
    using PrepareRefinedPreviewFunction = PrepareStatus(WINAPI*)(
        std::uint64_t lease_token,
        const wchar_t* language_tag,
        const PreviewPreparationOptions* options,
        PreparedPreview* preview) noexcept;
    using QueryPreviewNoticeFunction = BOOL(WINAPI*)(
        std::uint64_t lease_token,
        const wchar_t* language_tag,
        ComponentLoadingTextResult* result) noexcept;
    using QueryWebPreviewFunction = BOOL(WINAPI*)(
        std::uint64_t lease_token,
        const WebPreviewOptions* options,
        WebPreviewDescriptor* descriptor) noexcept;
    using QueryPagedDocumentHostFunction = BOOL(WINAPI*)(
        PagedDocumentHostDescriptor* descriptor) noexcept;
    using EnumerateComponentSettingsFunction = BOOL(WINAPI*)(
        const wchar_t* language_tag,
        ComponentSettingDescriptor* descriptors,
        std::uint32_t capacity,
        std::uint32_t* count) noexcept;
    using OpenFileDirectoryFunction = FileDirectoryOpenStatus(WINAPI*)(
        std::uint64_t lease_token,
        const wchar_t* language_tag,
        const wchar_t* password,
        FileDirectoryDescriptor* descriptor) noexcept;
    using EnumerateFileDirectoryChildrenFunction = BOOL(WINAPI*)(
        std::uint64_t lease_token,
        std::uint64_t parent_node_id,
        std::uint32_t offset,
        std::uint32_t limit,
        const FileDirectoryEntrySink* sink,
        std::uint32_t* returned,
        std::uint32_t* total) noexcept;
    using ClassifyGalleryExtensionFunction = GalleryMediaKind(WINAPI*)(
        const wchar_t* extension) noexcept;
    using QueryImageMetadataFunction = BOOL(WINAPI*)(
        std::uint64_t lease_token,
        const ImageMetadataSink* sink) noexcept;
    using QueryHoverInfoFunction = PrepareStatus(WINAPI*)(
        const wchar_t* hover_info_id,
        const wchar_t* path,
        const wchar_t* language_tag,
        const HoverInfoTextSink* sink) noexcept;
    using EnumerateStatusBarShortcutsFunction = BOOL(WINAPI*)(
        const wchar_t* language_tag,
        StatusBarShortcutDescriptor* descriptors,
        std::uint32_t capacity,
        std::uint32_t* count) noexcept;
    using QueryStatusBarShortcutStateFunction = StatusBarShortcutState(WINAPI*)(
        const wchar_t* shortcut_id,
        const wchar_t* path,
        PreviewContentKind kind,
        PreviewContentFormat format) noexcept;
    using ActivateStatusBarShortcutFunction = BOOL(WINAPI*)(
        const wchar_t* shortcut_id,
        const wchar_t* path,
        const wchar_t* language_tag,
        BOOL requested_checked,
        StatusBarShortcutActivationResult* result) noexcept;
    using QueryStatusBarShortcutDataFunction = PrepareStatus(WINAPI*)(
        const wchar_t* shortcut_id,
        const wchar_t* path,
        const HoverInfoTextSink* sink) noexcept;
    using EnumerateComponentManagementActionsFunction = BOOL(WINAPI*)(
        const wchar_t* language_tag,
        ComponentManagementActionDescriptor* descriptors,
        std::uint32_t capacity,
        std::uint32_t* count) noexcept;
    using PrepareComponentManagementActionFunction = BOOL(WINAPI*)(
        const wchar_t* action_id,
        const wchar_t* language_tag,
        ComponentDownloadRequest* request) noexcept;
    using CompleteComponentManagementActionFunction = BOOL(WINAPI*)(
        const wchar_t* action_id,
        const wchar_t* downloaded_path,
        const wchar_t* component_storage_path,
        const wchar_t* language_tag,
        ComponentManagementActionResult* result) noexcept;
    using QueryInterfaceFunction = BOOL(WINAPI*)(
        const GUID* interface_id,
        std::uint32_t minimum_version,
        void** interface_pointer) noexcept;
    using ShutdownFunction = void(WINAPI*)() noexcept;

    struct ConfigurablePreviewApi
    {
        std::uint32_t size{ sizeof(ConfigurablePreviewApi) };
        std::uint32_t version{ configurable_preview_api_version };
        PreparePreviewWithOptionsFunction prepare_preview{};
    };

    struct ProgressivePreviewApi
    {
        std::uint32_t size{ sizeof(ProgressivePreviewApi) };
        std::uint32_t version{ progressive_preview_api_version };
        CanRefinePreviewFunction can_refine{};
        QueryRefinementTextFunction query_refinement_text{};
        PrepareRefinedPreviewFunction prepare_refined_preview{};
    };

    struct PreviewNoticeApi
    {
        std::uint32_t size{ sizeof(PreviewNoticeApi) };
        std::uint32_t version{ preview_notice_api_version };
        QueryPreviewNoticeFunction query_preview_notice{};
    };

    struct WebPreviewApi
    {
        std::uint32_t size{ sizeof(WebPreviewApi) };
        std::uint32_t version{ web_preview_api_version };
        QueryWebPreviewFunction query_preview{};
    };

    struct PagedDocumentRendererApi
    {
        std::uint32_t size{ sizeof(PagedDocumentRendererApi) };
        std::uint32_t version{ paged_document_renderer_api_version };
        QueryPagedDocumentHostFunction query_host{};
    };

    struct SettingsContributionApi
    {
        std::uint32_t size{ sizeof(SettingsContributionApi) };
        std::uint32_t version{ settings_contribution_api_version };
        EnumerateComponentSettingsFunction enumerate_settings{};
    };

    struct FileDirectoryPreviewApi
    {
        std::uint32_t size{ sizeof(FileDirectoryPreviewApi) };
        std::uint32_t version{ file_directory_preview_api_version };
        OpenFileDirectoryFunction open{};
        EnumerateFileDirectoryChildrenFunction enumerate_children{};
    };

    struct GalleryMediaApi
    {
        std::uint32_t size{ sizeof(GalleryMediaApi) };
        std::uint32_t version{ gallery_media_api_version };
        ClassifyGalleryExtensionFunction classify_extension{};
    };

    struct ImageMetadataApi
    {
        std::uint32_t size{ sizeof(ImageMetadataApi) };
        std::uint32_t version{ image_metadata_api_version };
        QueryImageMetadataFunction query_metadata{};
    };

    struct HoverInfoLayerApi
    {
        std::uint32_t size{ sizeof(HoverInfoLayerApi) };
        std::uint32_t version{ hover_info_layer_api_version };
        QueryHoverInfoFunction query_info{};
    };

    struct StatusBarShortcutApi
    {
        std::uint32_t size{ sizeof(StatusBarShortcutApi) };
        std::uint32_t version{ status_bar_shortcut_api_version };
        EnumerateStatusBarShortcutsFunction enumerate_shortcuts{};
        QueryStatusBarShortcutStateFunction query_state{};
        ActivateStatusBarShortcutFunction activate{};
    };

    struct StatusBarShortcutDataApi
    {
        std::uint32_t size{ sizeof(StatusBarShortcutDataApi) };
        std::uint32_t version{ status_bar_shortcut_data_api_version };
        QueryStatusBarShortcutDataFunction query_json{};
    };

    struct ComponentManagementActionApi
    {
        std::uint32_t size{ sizeof(ComponentManagementActionApi) };
        std::uint32_t version{ component_management_action_api_version };
        EnumerateComponentManagementActionsFunction enumerate_actions{};
        PrepareComponentManagementActionFunction prepare_action{};
        CompleteComponentManagementActionFunction complete_action{};
    };

    struct ComponentApi
    {
        std::uint32_t size{ sizeof(ComponentApi) };
        std::uint32_t abi{ abi_version };
        InitializeFunction initialize{};
        QueryStatusFunction query_status{};
        QueryLoadingTextFunction query_loading_text{};
        CanPreviewFunction can_preview{};
        PreparePreviewFunction prepare_preview{};
        ReleasePreviewFunction release_preview{};
        QueryInterfaceFunction query_interface{};
        ShutdownFunction shutdown{};
    };

    using GetApiFunction = BOOL(WINAPI*)(
        std::uint32_t host_abi,
        ComponentApi* api) noexcept;
}

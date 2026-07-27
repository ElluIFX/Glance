#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace glance::contracts::components
{
    inline constexpr std::uint32_t abi_version = 4;
    inline constexpr char get_api_export[] = "GlanceComponentGetApi";
    inline constexpr std::size_t component_id_capacity = 64;
    inline constexpr std::size_t target_app_version_capacity = 32;
    inline constexpr std::size_t display_name_capacity = 128;
    inline constexpr std::size_t status_detail_capacity = 256;
    inline constexpr std::size_t loading_text_capacity = 256;
    inline constexpr std::size_t preview_path_capacity = 32768;
    inline constexpr std::size_t preview_error_capacity = 256;

    enum class PreviewContentKind : std::uint32_t
    {
        none = 0,
        text = 1,
        image = 2,
        media = 3,
        document = 4,
        web = 5,
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
    };

    enum class PrepareStatus : std::uint32_t
    {
        success = 0,
        unavailable = 1,
        failed = 2,
        cancelled = 3,
    };

    enum class HealthSeverity : std::uint32_t
    {
        healthy = 0,
        warning = 1,
        error = 2,
    };

    using RegisterExtensionFunction = BOOL(WINAPI*)(
        void* context,
        const wchar_t* extension) noexcept;

    struct ComponentRegistrar
    {
        std::uint32_t size{ sizeof(ComponentRegistrar) };
        void* context{};
        RegisterExtensionFunction register_extension{};
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
    using QueryInterfaceFunction = BOOL(WINAPI*)(
        const GUID* interface_id,
        std::uint32_t minimum_version,
        void** interface_pointer) noexcept;
    using ShutdownFunction = void(WINAPI*)() noexcept;

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

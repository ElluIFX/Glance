#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace glance::contracts::components
{
    inline constexpr std::uint32_t abi_version = 2;
    inline constexpr char get_api_export[] = "GlanceComponentGetApi";
    inline constexpr std::size_t component_id_capacity = 64;
    inline constexpr std::size_t target_app_version_capacity = 32;
    inline constexpr std::size_t health_detail_capacity = 256;

    enum class PreviewOutputKind : std::uint32_t
    {
        none = 0,
        pdf_file = 1,
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

    struct HealthResult
    {
        std::uint32_t size{ sizeof(HealthResult) };
        HealthSeverity severity{ HealthSeverity::error };
        std::uint32_t code{};
        std::uint64_t capability_mask{};
        wchar_t detail[health_detail_capacity]{};
    };

    using QueryHealthFunction = BOOL(WINAPI*)(
        const wchar_t* language_tag,
        HealthResult* result) noexcept;
    using CanPreviewFunction = BOOL(WINAPI*)(const wchar_t* path) noexcept;
    using PreparePreviewFunction = PrepareStatus(WINAPI*)(
        const wchar_t* path,
        wchar_t* output_path,
        std::uint32_t output_path_capacity) noexcept;
    using ShutdownFunction = void(WINAPI*)() noexcept;

    struct ComponentApi
    {
        std::uint32_t size{ sizeof(ComponentApi) };
        std::uint32_t abi{ abi_version };
        wchar_t component_id[component_id_capacity]{};
        wchar_t target_app_version[target_app_version_capacity]{};
        PreviewOutputKind output_kind{ PreviewOutputKind::none };
        QueryHealthFunction query_health{};
        CanPreviewFunction can_preview{};
        PreparePreviewFunction prepare_preview{};
        ShutdownFunction shutdown{};
    };

    using GetApiFunction = BOOL(WINAPI*)(
        std::uint32_t host_abi,
        ComponentApi* api) noexcept;
}

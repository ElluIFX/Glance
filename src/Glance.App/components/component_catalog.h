#pragma once

#include "glance/contracts/component_api.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace glance::app
{
    struct SupportedComponent
    {
        std::wstring_view id;
        std::wstring_view display_name_resource;
        std::uint32_t abi_version{};
        std::wstring_view architecture;
        std::wstring_view entry_point;
        glance::contracts::components::PreviewOutputKind output_kind{
            glance::contracts::components::PreviewOutputKind::none };
        std::span<const std::wstring_view> extensions;
    };

    [[nodiscard]] std::span<const SupportedComponent> supported_components() noexcept;
    [[nodiscard]] const SupportedComponent* find_supported_component(
        std::wstring_view id) noexcept;
    [[nodiscard]] const SupportedComponent* find_component_for_extension(
        std::wstring_view extension) noexcept;
}

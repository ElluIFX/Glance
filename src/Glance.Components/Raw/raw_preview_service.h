#pragma once

#include "glance/contracts/component_api.h"

#include <cstdint>
#include <filesystem>

namespace glance::components::raw
{
    struct PreviewResult
    {
        glance::contracts::components::PrepareStatus status{
            glance::contracts::components::PrepareStatus::failed };
        glance::contracts::components::PreviewContentKind kind{
            glance::contracts::components::PreviewContentKind::none };
        glance::contracts::components::PreviewContentFormat format{
            glance::contracts::components::PreviewContentFormat::none };
        std::filesystem::path path;
        std::uint64_t lease_token{};
    };

    void initialize() noexcept;
    [[nodiscard]] bool can_preview(const std::filesystem::path& path) noexcept;
    [[nodiscard]] PreviewResult prepare_preview(
        const std::filesystem::path& path,
        std::uint32_t maximum_dimension) noexcept;
    [[nodiscard]] bool query_metadata(
        std::uint64_t lease_token,
        const glance::contracts::components::ImageMetadataSink* sink) noexcept;
    void release_preview(std::uint64_t lease_token) noexcept;
    void shutdown() noexcept;
}

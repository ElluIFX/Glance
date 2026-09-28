#pragma once

#include "glance/contracts/component_api.h"
#include "glance/contracts/dependency_api.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace glance::components::media_info
{
    [[nodiscard]] glance::contracts::components::PrepareStatus query_media_info(
        const glance::contracts::dependencies::HostApi& dependencies,
        std::wstring_view path,
        const glance::contracts::components::InformationPanelSink& sink) noexcept;
    [[nodiscard]] std::wstring query_media_json(
        const glance::contracts::dependencies::HostApi& dependencies,
        std::wstring_view path,
        const glance::contracts::components::HoverInfoTextSink& sink) noexcept;
}

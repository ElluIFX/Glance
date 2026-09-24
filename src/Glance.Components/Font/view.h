#pragma once
#include "glance/contracts/component_api.h"
#include <filesystem>

namespace glance::font
{
std::filesystem::path directory();
const glance::contracts::components::ComponentViewApi &view_api();
void shutdown_views() noexcept;
} // namespace glance::font

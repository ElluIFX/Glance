#pragma once
#include "glance/contracts/component_api.h"
namespace glance::executable
{
const contracts::components::ComponentViewApi& view_api();
void shutdown_views() noexcept;
}

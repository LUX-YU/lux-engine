#pragma once

#include <lux/engine/ecs/ComponentSchema.hpp>
#include <lux/engine/ecs/hierarchy/visibility.h>

#include <span>

namespace lux::ecs
{
    [[nodiscard]] LUX_ENGINE_ECS_HIERARCHY_PUBLIC
    std::span<const ComponentSchema> hierarchyComponentSchemas();
} // namespace lux::ecs

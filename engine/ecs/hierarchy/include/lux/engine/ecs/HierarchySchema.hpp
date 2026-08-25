#pragma once

#include <lux/engine/ecs/ComponentSchema.hpp>
#include <lux/engine/ecs/persistence/ComponentPersistenceBinding.hpp>
#include <lux/engine/ecs/hierarchy/visibility.h>

#include <span>

namespace lux::ecs
{
    [[nodiscard]] LUX_ENGINE_ECS_HIERARCHY_PUBLIC
    std::span<const ComponentSchema> hierarchyComponentSchemas();

    [[nodiscard]] LUX_ENGINE_ECS_HIERARCHY_PUBLIC
    ComponentPersistenceContribution hierarchyPersistenceContribution() noexcept;
} // namespace lux::ecs

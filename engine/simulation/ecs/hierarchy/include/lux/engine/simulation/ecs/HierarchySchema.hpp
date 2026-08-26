#pragma once

#include <lux/engine/simulation/ecs/ComponentSchema.hpp>
#include <lux/engine/simulation/ecs/ComponentSnapshotBinding.hpp>
#include <lux/engine/simulation/ecs/hierarchy/visibility.h>

#include <span>

namespace lux::simulation::ecs
{
    [[nodiscard]] LUX_ENGINE_SIMULATION_ECS_HIERARCHY_PUBLIC
    std::span<const ComponentSchema> hierarchyComponentSchemas();

    [[nodiscard]] LUX_ENGINE_SIMULATION_ECS_HIERARCHY_PUBLIC
    ComponentSnapshotContribution
    hierarchyComponentSnapshotContribution() noexcept;
} // namespace lux::simulation::ecs

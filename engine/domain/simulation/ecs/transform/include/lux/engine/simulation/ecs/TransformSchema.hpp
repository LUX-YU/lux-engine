#pragma once

#include <lux/engine/simulation/ecs/ComponentSchema.hpp>
#include <lux/engine/simulation/ecs/ComponentSnapshotBinding.hpp>
#include <lux/engine/simulation/ecs/transform/visibility.h>

#include <span>

namespace lux::simulation::ecs
{
    [[nodiscard]] LUX_ENGINE_SIMULATION_ECS_TRANSFORM_PUBLIC std::span<const ComponentSchema>
    transformComponentSchemas() noexcept;

    [[nodiscard]] LUX_ENGINE_SIMULATION_ECS_TRANSFORM_PUBLIC ComponentSnapshotContribution
    transformComponentSnapshotContribution() noexcept;
} // namespace lux::simulation::ecs

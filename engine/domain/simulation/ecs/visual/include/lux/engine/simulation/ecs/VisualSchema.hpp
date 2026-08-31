#pragma once

#include <lux/engine/simulation/ecs/ComponentSchema.hpp>
#include <lux/engine/simulation/ecs/ComponentSnapshotBinding.hpp>
#include <lux/engine/simulation/ecs/visual/visibility.h>

#include <span>

namespace lux::simulation::ecs
{
    [[nodiscard]] LUX_ENGINE_SIMULATION_ECS_VISUAL_PUBLIC std::span<const ComponentSchema>
    visualComponentSchemas() noexcept;

    [[nodiscard]] LUX_ENGINE_SIMULATION_ECS_VISUAL_PUBLIC ComponentSnapshotContribution
    visualComponentSnapshotContribution() noexcept;
}

#pragma once

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/physics/CollisionProbe2D.hpp>
#include <lux/engine/ecs/physics/systems/Simulation2DSystem.hpp>

#include <memory>

namespace lux::ecs
{
    [[nodiscard]] inline bool installSimulation2DSystems(
        ScheduleBuilder& builder,
        const ComponentTypeCatalog&)
    {
        const auto checkpoint = builder.checkpoint();
        if (!checkpoint.valid())
            return false;
        const auto* configured = builder.services().get<FixedStepConfig>();
        auto system = std::make_unique<Simulation2DSystem>(
            configured ? *configured : FixedStepConfig{});
        auto* const owner = system.get();
        if (!builder.add(std::move(system)) ||
            !builder.services().adopt(*owner) ||
            !builder.services().emplace<CollisionProbes2D>())
        {
            (void)builder.rollbackTo(checkpoint);
            return false;
        }
        return true;
    }
}

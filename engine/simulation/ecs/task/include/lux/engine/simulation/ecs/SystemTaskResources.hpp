#pragma once

#include <lux/engine/simulation/ecs/EcsTaskResource.hpp>
#include <lux/engine/simulation/SystemAccessSpec.hpp>
#include <lux/engine/simulation/SystemConcept.hpp>
#include <lux/engine/task/Task.hpp>

#include <cstdint>

namespace lux::simulation::ecs
{
    /** Converts ECS access metadata into generic L0 Task resource metadata. */
    [[nodiscard]] inline task::TaskResources systemTaskResources(
        SystemAccessSpec access
    )
    {
        task::TaskResources result;
        result.values.reserve(access.components.size() + access.external.size());

        for (const auto& component : access.components)
        {
            const auto key = componentTaskResource(component.storage);
            result.values.push_back(
                component.mode == ESystemAccessMode::WRITE
                    ? task::write(key)
                    : task::read(key)
            );
        }
        for (const auto& external : access.external)
        {
            const auto key = externalTaskResource(external.type.hash());
            result.values.push_back(
                external.mode == ESystemAccessMode::WRITE
                    ? task::write(key)
                    : task::read(key)
            );
        }
        return result;
    }

    template <System Type>
    [[nodiscard]] task::TaskResources systemTaskResources()
    {
        return systemTaskResources(Type::Access.spec());
    }
}

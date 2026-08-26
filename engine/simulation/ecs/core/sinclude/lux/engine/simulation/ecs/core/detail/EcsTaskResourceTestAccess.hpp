#pragma once

#include <lux/engine/simulation/ecs/EcsTaskResources.hpp>

namespace lux::simulation::ecs::detail
{
    struct LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC EcsTaskResourceTestAccess final
    {
        static void failNextPush(
            EcsCommandBatch& batch,
            std::size_t producer
        ) noexcept;
    };
}

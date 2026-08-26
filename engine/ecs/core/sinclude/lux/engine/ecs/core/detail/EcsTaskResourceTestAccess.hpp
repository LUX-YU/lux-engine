#pragma once

#include <lux/engine/ecs/EcsTaskResources.hpp>

namespace lux::ecs::detail
{
    struct LUX_ENGINE_ECS_CORE_PUBLIC EcsTaskResourceTestAccess final
    {
        static void failNextPush(
            EcsCommandBatch& batch,
            std::size_t producer
        ) noexcept;
    };
}

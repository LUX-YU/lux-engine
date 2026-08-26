#pragma once

#include <lux/engine/ecs/WorldTaskResources.hpp>

namespace lux::ecs::detail
{
    struct LUX_ENGINE_ECS_CORE_PUBLIC WorldTaskResourceTestAccess final
    {
        static void failNextPush(
            WorldCommandBatch& batch,
            std::size_t producer
        ) noexcept;
    };
}

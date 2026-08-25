#pragma once

#include <lux/engine/ecs/World.hpp>

namespace lux::ecs::detail
{
    struct PersistenceStorageAccess final
    {
        template <class Component>
        [[nodiscard]] static const auto* storage(const World& world) noexcept
        {
            return world.registry_.template storage<Component>();
        }
    };
} // namespace lux::ecs::detail

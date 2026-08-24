#pragma once

#include <lux/engine/ecs/World.hpp>

namespace lux::ecs::detail
{
    struct WorldEditAccess final
    {
        [[nodiscard]] static World& world(WorldEdit& edit) noexcept
        {
            require(edit.world_ != nullptr);
            return *edit.world_;
        }
    };
} // namespace lux::ecs::detail

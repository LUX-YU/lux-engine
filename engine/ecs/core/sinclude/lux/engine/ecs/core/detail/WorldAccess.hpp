#pragma once

#include <lux/engine/ecs/World.hpp>

#include <thread>

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

    struct WorldColdAccess final
    {
        [[nodiscard]] static bool ownerIdle(const World& world) noexcept
        {
            return world.state_ == EWorldState::IDLE &&
                world.owner_thread_ == std::this_thread::get_id();
        }
    };
} // namespace lux::ecs::detail

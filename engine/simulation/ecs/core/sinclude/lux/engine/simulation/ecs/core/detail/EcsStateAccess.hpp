#pragma once

#include <lux/engine/simulation/ecs/EcsState.hpp>
#include <lux/engine/simulation/ecs/core/detail/EcsChangeLog.hpp>

#include <thread>

namespace lux::simulation::ecs::detail
{
    enum class EEntityReferenceState : std::uint8_t
    {
        CURRENT,
        STALE,
        UNKNOWN,
    };

    struct EcsMutationAccess final
    {
        [[nodiscard]] static EcsState& world(EcsMutation& edit) noexcept
        {
            require(edit.world_ != nullptr);
            return *edit.world_;
        }
    };

    struct EcsColdAccess final
    {
        [[nodiscard]] static bool ownerIdle(const EcsState& world) noexcept
        {
            return world.state_ == EEcsState::IDLE &&
                world.owner_thread_ == std::this_thread::get_id();
        }

        [[nodiscard]] static EcsMutation mutation(
            EcsState& world
        ) noexcept
        {
            require(ownerIdle(world));
            world.state_ = EEcsState::MUTATING;
            return EcsMutation(world, true);
        }

    };

    struct EcsEntityAccess final
    {
        [[nodiscard]] static EEntityReferenceState referenceState(
            const EcsState& world,
            Entity entity
        ) noexcept
        {
            if (world.registry_.valid(entity))
                return EEntityReferenceState::CURRENT;
            if (world.registry_.current(entity) !=
                entt::entt_traits<Entity>::to_version(entt::tombstone))
            {
                return EEntityReferenceState::STALE;
            }
            return EEntityReferenceState::UNKNOWN;
        }
    };
} // namespace lux::simulation::ecs::detail

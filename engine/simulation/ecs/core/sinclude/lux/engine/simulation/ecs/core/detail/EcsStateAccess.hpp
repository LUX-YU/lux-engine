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
        [[nodiscard]] static EcsState& state(EcsMutation& mutation) noexcept
        {
            require(mutation.state_ != nullptr);
            return *mutation.state_;
        }
    };

    struct EcsColdAccess final
    {
        [[nodiscard]] static bool ownerIdle(const EcsState& state) noexcept
        {
            return state.state_ == EEcsState::IDLE &&
                state.owner_thread_ == std::this_thread::get_id();
        }

        [[nodiscard]] static EcsMutation mutation(
            EcsState& state
        ) noexcept
        {
            require(ownerIdle(state));
            state.state_ = EEcsState::MUTATING;
            return EcsMutation(state, true);
        }

    };

    struct EcsEntityAccess final
    {
        [[nodiscard]] static EEntityReferenceState referenceState(
            const EcsState& state,
            Entity entity
        ) noexcept
        {
            if (state.registry_.valid(entity))
                return EEntityReferenceState::CURRENT;
            if (state.registry_.current(entity) !=
                entt::entt_traits<Entity>::to_version(entt::tombstone))
            {
                return EEntityReferenceState::STALE;
            }
            return EEntityReferenceState::UNKNOWN;
        }
    };
} // namespace lux::simulation::ecs::detail

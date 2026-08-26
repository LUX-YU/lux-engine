#pragma once

#include <lux/engine/ecs/EcsState.hpp>
#include <lux/engine/ecs/core/detail/EcsChangeLog.hpp>

#include <thread>

namespace lux::ecs::detail
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
                !world.execution_lease_ &&
                world.owner_thread_ == std::this_thread::get_id();
        }

        [[nodiscard]] static EcsMutation suppressingMutation(
            EcsState& world
        ) noexcept
        {
            require(ownerIdle(world));
            world.state_ = EEcsState::MUTATING;
            return EcsMutation(
                world,
                true,
                EcsMutation::EChangeEmission::SUPPRESS
            );
        }

    };

    class EcsChangePublisher final
    {
      public:
        explicit EcsChangePublisher(EcsState& world) noexcept
            : log_(&EcsChangeAccess::log(world))
        {
        }

        [[nodiscard]] BoundEcsChangeStream bindComponent(
            std::uint64_t storage
        ) noexcept
        {
            if (!exact_)
                return {};
            BoundEcsChangeStream result = log_->bindComponent(storage);
            if (!result)
                exact_ = false;
            return result;
        }

        [[nodiscard]] bool append(
            BoundEcsChangeStream stream,
            Entity entity,
            EComponentChangeKind kind
        ) noexcept
        {
            if (!exact_)
                return false;
            exact_ = stream(entity, kind);
            return exact_;
        }

        [[nodiscard]] bool appendEntity(
            Entity entity,
            EEntityChangeKind kind
        ) noexcept
        {
            if (!exact_)
                return false;
            exact_ = log_->recordEntity(entity, kind);
            return exact_;
        }

        [[nodiscard]] bool exact() const noexcept
        {
            return exact_;
        }

      private:
        EcsChangeLog* log_{};
        bool exact_{true};
    };

    struct EcsExecutionAccess final
    {
        [[nodiscard]] static bool acquire(EcsState& world) noexcept
        {
            if (world.owner_thread_ != std::this_thread::get_id() ||
                world.state_ != EEcsState::IDLE ||
                world.execution_lease_)
            {
                return false;
            }
            world.execution_lease_ = true;
            world.state_ = EEcsState::EXECUTING;
            return true;
        }

        static void beginApplyingCommands(EcsState& world) noexcept
        {
            require(world.execution_lease_);
            require(world.state_ == EEcsState::EXECUTING);
            world.state_ = EEcsState::APPLYING_COMMANDS;
        }

        [[nodiscard]] static EcsMutation commandMutation(
            EcsState& world
        ) noexcept
        {
            require(world.execution_lease_);
            require(world.state_ == EEcsState::APPLYING_COMMANDS);
            return EcsMutation(world, false);
        }

        static void resume(EcsState& world) noexcept
        {
            require(world.execution_lease_);
            require(world.state_ == EEcsState::APPLYING_COMMANDS);
            world.state_ = EEcsState::EXECUTING;
        }

        static void release(EcsState& world) noexcept
        {
            require(world.execution_lease_);
            require(
                world.state_ == EEcsState::EXECUTING ||
                world.state_ == EEcsState::APPLYING_COMMANDS
            );
            world.state_ = EEcsState::IDLE;
            world.execution_lease_ = false;
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
} // namespace lux::ecs::detail

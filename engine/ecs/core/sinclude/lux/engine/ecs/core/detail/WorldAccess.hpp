#pragma once

#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/core/detail/WorldChangeLog.hpp>

#include <limits>
#include <thread>

namespace lux::ecs::detail
{
    enum class EEntityReferenceState : std::uint8_t
    {
        CURRENT,
        STALE,
        UNKNOWN,
    };

    struct WorldMutationAccess final
    {
        [[nodiscard]] static World& world(WorldMutation& edit) noexcept
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
                !world.execution_lease_ &&
                world.owner_thread_ == std::this_thread::get_id();
        }

        [[nodiscard]] static WorldMutation suppressingMutation(
            World& world
        ) noexcept
        {
            require(ownerIdle(world));
            world.state_ = EWorldState::MUTATING;
            return WorldMutation(
                world,
                true,
                WorldMutation::EChangeEmission::SUPPRESS
            );
        }

        [[nodiscard]] static WorldMutation sectionMutation(
            World& world
        ) noexcept
        {
            require(ownerIdle(world));
            world.state_ = EWorldState::MUTATING;
            return WorldMutation(
                world,
                true,
                WorldMutation::EChangeEmission::SUPPRESS
            );
        }

        [[nodiscard]] static std::uint64_t identity(
            const World& world
        ) noexcept
        {
            return world.identity_;
        }

        [[nodiscard]] static std::size_t activeSectionCount(
            const World& world
        ) noexcept
        {
            return world.active_section_count_;
        }

        static void acquireSection(World& world) noexcept
        {
            require(
                world.active_section_count_ !=
                std::numeric_limits<std::size_t>::max()
            );
            ++world.active_section_count_;
        }

        static void releaseSection(World& world) noexcept
        {
            require(world.active_section_count_ != 0U);
            --world.active_section_count_;
        }
    };

    class WorldChangePublisher final
    {
      public:
        explicit WorldChangePublisher(World& world) noexcept
            : log_(&WorldChangeAccess::log(world))
        {
        }

        [[nodiscard]] BoundWorldChangeStream bindComponent(
            std::uint64_t storage
        ) noexcept
        {
            if (!exact_)
                return {};
            BoundWorldChangeStream result = log_->bindComponent(storage);
            if (!result)
                exact_ = false;
            return result;
        }

        [[nodiscard]] bool append(
            BoundWorldChangeStream stream,
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
        WorldChangeLog* log_{};
        bool exact_{true};
    };

    struct WorldExecutionAccess final
    {
        [[nodiscard]] static bool acquire(World& world) noexcept
        {
            if (world.owner_thread_ != std::this_thread::get_id() ||
                world.state_ != EWorldState::IDLE ||
                world.execution_lease_)
            {
                return false;
            }
            world.execution_lease_ = true;
            world.state_ = EWorldState::EXECUTING;
            return true;
        }

        static void beginApplyingCommands(World& world) noexcept
        {
            require(world.execution_lease_);
            require(world.state_ == EWorldState::EXECUTING);
            world.state_ = EWorldState::APPLYING_COMMANDS;
        }

        [[nodiscard]] static WorldMutation commandMutation(
            World& world
        ) noexcept
        {
            require(world.execution_lease_);
            require(world.state_ == EWorldState::APPLYING_COMMANDS);
            return WorldMutation(world, false);
        }

        static void resume(World& world) noexcept
        {
            require(world.execution_lease_);
            require(world.state_ == EWorldState::APPLYING_COMMANDS);
            world.state_ = EWorldState::EXECUTING;
        }

        static void release(World& world) noexcept
        {
            require(world.execution_lease_);
            require(
                world.state_ == EWorldState::EXECUTING ||
                world.state_ == EWorldState::APPLYING_COMMANDS
            );
            world.state_ = EWorldState::IDLE;
            world.execution_lease_ = false;
        }
    };

    struct WorldEntityAccess final
    {
        [[nodiscard]] static EEntityReferenceState referenceState(
            const World& world,
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

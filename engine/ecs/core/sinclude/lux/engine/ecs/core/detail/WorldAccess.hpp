#pragma once

#include <lux/engine/ecs/World.hpp>

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

        [[nodiscard]] static WorldEdit suppressingEdit(World& world) noexcept
        {
            require(ownerIdle(world));
            require(world.schedule_ == nullptr);
            world.state_ = EWorldState::EDITING;
            return WorldEdit(
                world,
                true,
                WorldEdit::EChangeEmission::SUPPRESS
            );
        }

        [[nodiscard]] static WorldEdit sectionEdit(World& world) noexcept
        {
            require(ownerIdle(world));
            world.state_ = EWorldState::EDITING;
            return WorldEdit(
                world,
                true,
                WorldEdit::EChangeEmission::SUPPRESS
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

#pragma once

#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/core/detail/SectionMembershipDirectory.hpp>

#include <iterator>
#include <span>
#include <vector>

namespace lux::ecs::detail
{
    struct WorldSectionTransactionAccess final
    {
        [[nodiscard]] static std::uint64_t allocateLease(
            WorldEdit& edit
        ) noexcept
        {
            require(edit.world_ != nullptr);
            return edit.world_->section_memberships_->allocateLease();
        }

        static void reserveMembership(
            WorldEdit& edit,
            std::span<const Entity> entities,
            std::size_t additional_memberships
        )
        {
            require(edit.world_ != nullptr);
            edit.world_->section_memberships_->reserve(
                entities,
                additional_memberships
            );
        }

        static void activateMembership(
            WorldEdit& edit,
            std::uint64_t lease,
            std::span<const Entity> entities
        ) noexcept
        {
            require(edit.world_ != nullptr);
            edit.world_->section_memberships_->activate(lease, entities);
        }

        static void addComponentMembership(
            WorldEdit& edit,
            std::uint64_t storage,
            std::span<const Entity> entities
        ) noexcept
        {
            require(edit.world_ != nullptr);
            for (const Entity entity : entities)
            {
                edit.world_->section_memberships_->addReserved(
                    entity,
                    storage
                );
            }
        }

        [[nodiscard]] static bool matches(
            WorldEdit& edit,
            Entity entity,
            std::uint64_t lease
        ) noexcept
        {
            require(edit.world_ != nullptr);
            return edit.world_->registry_.valid(entity) &&
                edit.world_->section_memberships_->matches(entity, lease);
        }

        template <class Fn>
        static void forEachStorage(
            WorldEdit& edit,
            Entity entity,
            Fn&& fn
        ) noexcept
        {
            require(edit.world_ != nullptr);
            edit.world_->section_memberships_->forEachStorage(
                entity,
                std::forward<Fn>(fn)
            );
        }

        static void removeComponent(
            WorldEdit& edit,
            Entity entity,
            std::uint64_t storage
        ) noexcept
        {
            require(edit.world_ != nullptr);
            auto* component_storage = edit.world_->registry_.storage(storage);
            require(component_storage != nullptr);
            component_storage->remove(entity);
        }

        static void destroyTrackedEntity(
            WorldEdit& edit,
            Entity entity
        ) noexcept
        {
            require(edit.world_ != nullptr);
            edit.world_->section_memberships_->deactivate(entity);
            edit.world_->registry_.template storage<Entity>().erase(entity);
        }

        static void createEntities(
            WorldEdit& edit,
            std::span<Entity> entities
        )
        {
            require(edit.world_ != nullptr);
            edit.world_->registry_.create(entities.begin(), entities.end());
        }

#if defined(LUX_ECS_WORLD_SECTION_TESTING)
        template <class Component>
        static void insertPredecodedForBenchmark(
            WorldEdit& edit,
            std::span<const Entity> entities,
            std::vector<Component>& values
        )
        {
            require(edit.world_ != nullptr);
            require(entities.size() == values.size());
            auto& storage =
                edit.world_->registry_.template storage<Component>();
            storage.reserve(storage.size() + entities.size());
            storage.insert(
                entities.begin(),
                entities.end(),
                std::make_move_iterator(values.begin())
            );
        }
#endif

        static void destroyEntities(
            WorldEdit& edit,
            std::span<const Entity> entities
        )
        {
            require(edit.world_ != nullptr);
            edit.world_->registry_.destroy(entities.begin(), entities.end());
        }

        static void destroyBareEntities(
            WorldEdit& edit,
            std::span<const Entity> entities
        ) noexcept
        {
            require(edit.world_ != nullptr);
            auto& entity_storage =
                edit.world_->registry_.template storage<Entity>();
            for (const Entity entity : entities)
            {
                if (edit.world_->registry_.valid(entity))
                    entity_storage.erase(entity);
            }
        }

        static void destroyValidEntities(
            WorldEdit& edit,
            std::span<const Entity> entities
        ) noexcept
        {
            require(edit.world_ != nullptr);
            for (const Entity entity : entities)
            {
                if (edit.world_->registry_.valid(entity))
                    edit.world_->registry_.destroy(entity);
            }
        }

        static void rollbackEntities(
            WorldEdit& edit,
            std::uint64_t lease,
            std::span<const Entity> entities
        ) noexcept
        {
            require(edit.world_ != nullptr);
            for (const Entity entity : entities)
            {
                if (!matches(edit, entity, lease))
                    continue;
                forEachStorage(
                    edit,
                    entity,
                    [&](std::uint64_t storage) noexcept
                    {
                        removeComponent(edit, entity, storage);
                    }
                );
                destroyTrackedEntity(edit, entity);
            }
        }
    };
} // namespace lux::ecs::detail

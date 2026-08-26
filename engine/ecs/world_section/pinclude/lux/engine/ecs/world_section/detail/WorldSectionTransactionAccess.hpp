#pragma once

#include <lux/engine/ecs/EcsState.hpp>
#include <lux/engine/ecs/core/detail/SectionMembershipDirectory.hpp>

#include <iterator>
#include <span>
#include <vector>

namespace lux::ecs::detail
{
    struct WorldSectionTransactionAccess final
    {
        struct EntityAllocatorCheckpoint final
        {
            std::size_t packed_size{};
            std::size_t free_list{};
        };

        [[nodiscard]] static EntityAllocatorCheckpoint checkpointAllocator(
            EcsMutation& mutation
        ) noexcept
        {
            require(mutation.world_ != nullptr);
            const auto& storage =
                mutation.world_->registry_.template storage<Entity>();
            return {storage.size(), storage.free_list()};
        }

        [[nodiscard]] static std::uint64_t allocateLease(
            EcsMutation& edit
        ) noexcept
        {
            require(edit.world_ != nullptr);
            return edit.world_->section_memberships_->allocateLease();
        }

        static void reserveMembership(
            EcsMutation& edit,
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
            EcsMutation& edit,
            std::uint64_t lease,
            std::span<const Entity> entities
        ) noexcept
        {
            require(edit.world_ != nullptr);
            edit.world_->section_memberships_->activate(lease, entities);
        }

        static void addComponentMembership(
            EcsMutation& mutation,
            std::uint64_t storage,
            std::span<const Entity> entities
        ) noexcept
        {
            require(mutation.world_ != nullptr);
            for (const Entity entity : entities)
            {
                mutation.world_->section_memberships_->appendKnownUnique(
                    entity,
                    storage
                );
            }
        }

        [[nodiscard]] static bool matches(
            EcsMutation& edit,
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
            EcsMutation& edit,
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
            EcsMutation& edit,
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
            EcsMutation& edit,
            Entity entity
        ) noexcept
        {
            require(edit.world_ != nullptr);
            edit.world_->section_memberships_->deactivate(entity);
            edit.world_->registry_.template storage<Entity>().erase(entity);
        }

        static void createEntities(
            EcsMutation& edit,
            std::span<Entity> entities
        )
        {
            require(edit.world_ != nullptr);
            edit.world_->registry_.create(entities.begin(), entities.end());
        }

#if defined(LUX_ECS_WORLD_SECTION_TESTING)
        template <class Component>
        static void insertPredecodedForBenchmark(
            EcsMutation& edit,
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
            EcsMutation& edit,
            std::span<const Entity> entities
        )
        {
            require(edit.world_ != nullptr);
            edit.world_->registry_.destroy(entities.begin(), entities.end());
        }

        static void destroyBareEntities(
            EcsMutation& edit,
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
            EcsMutation& edit,
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
            EcsMutation& mutation,
            std::uint64_t lease,
            std::span<const Entity> entities,
            EntityAllocatorCheckpoint checkpoint,
            bool membership_active
        ) noexcept
        {
            require(mutation.world_ != nullptr);
            auto& entity_storage =
                mutation.world_->registry_.template storage<Entity>();
            for (auto iterator = entities.rbegin();
                 iterator != entities.rend();
                 ++iterator)
            {
                const Entity entity = *iterator;
                if (!mutation.world_->registry_.valid(entity))
                    continue;
                if (membership_active && matches(mutation, entity, lease))
                {
                    forEachStorage(
                        mutation,
                        entity,
                        [&](std::uint64_t storage) noexcept
                        {
                            removeComponent(mutation, entity, storage);
                        }
                    );
                    mutation.world_->section_memberships_->deactivate(entity);
                }
                entity_storage.erase(entity);
                entity_storage.bump(entity);
            }
            entity_storage.free_list(checkpoint.free_list);
            using Traits = entt::entt_traits<Entity>;
            entity_storage.start_from(Traits::construct(
                static_cast<Traits::entity_type>(checkpoint.packed_size),
                {}
            ));
        }

        [[nodiscard]] static SectionMembershipDirectory::Stats membershipStats(
            const EcsState& world
        ) noexcept
        {
            return world.section_memberships_->stats();
        }
    };
} // namespace lux::ecs::detail

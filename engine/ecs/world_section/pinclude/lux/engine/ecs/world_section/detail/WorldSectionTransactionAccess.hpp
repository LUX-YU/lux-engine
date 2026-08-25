#pragma once

#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/core/detail/SectionResidencyDirectory.hpp>

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
            return edit.world_->section_residencies_->allocateLease();
        }

        static void reserveResidency(
            WorldEdit& edit,
            std::span<const Entity> entities,
            std::size_t additional_owners
        )
        {
            require(edit.world_ != nullptr);
            edit.world_->section_residencies_->reserve(
                entities,
                additional_owners
            );
        }

        static void activateResidency(
            WorldEdit& edit,
            std::uint64_t lease,
            std::shared_ptr<void> owner,
            void* context,
            const SectionResidencyPort& port,
            std::span<const Entity> entities
        ) noexcept
        {
            require(edit.world_ != nullptr);
            edit.world_->section_residencies_->activate(
                lease,
                std::move(owner),
                context,
                port,
                entities
            );
        }

        [[nodiscard]] static bool matches(
            WorldEdit& edit,
            Entity entity,
            std::uint64_t lease
        ) noexcept
        {
            require(edit.world_ != nullptr);
            return edit.world_->registry_.valid(entity) &&
                edit.world_->section_residencies_->matches(entity, lease);
        }

        static void removeComponent(
            WorldEdit& edit,
            Entity entity,
            std::uint64_t storage
        ) noexcept
        {
            require(edit.world_ != nullptr);
            auto* component_storage = edit.world_->registry_.storage(storage);
            if (component_storage != nullptr)
                component_storage->remove(entity);
        }

        [[nodiscard]] static bool hasComponent(
            WorldEdit& edit,
            Entity entity,
            std::uint64_t storage
        ) noexcept
        {
            require(edit.world_ != nullptr);
            const auto* component_storage = edit.world_->registry_.storage(
                storage
            );
            return component_storage != nullptr &&
                component_storage->contains(entity);
        }

        static void deactivateEntity(
            WorldEdit& edit,
            Entity entity
        ) noexcept
        {
            require(edit.world_ != nullptr);
            edit.world_->section_residencies_->deactivate(entity);
        }

        static void destroyTrackedEntity(
            WorldEdit& edit,
            Entity entity
        ) noexcept
        {
            require(edit.world_ != nullptr);
            deactivateEntity(edit, entity);
            edit.world_->registry_.template storage<Entity>().erase(entity);
        }

        static void releaseResidency(
            WorldEdit& edit,
            std::uint64_t lease
        ) noexcept
        {
            require(edit.world_ != nullptr);
            edit.world_->section_residencies_->release(lease);
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
    };
} // namespace lux::ecs::detail

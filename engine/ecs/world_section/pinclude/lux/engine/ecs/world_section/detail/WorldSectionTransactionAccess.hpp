#pragma once

#include <lux/engine/ecs/World.hpp>

#include <span>

namespace lux::ecs::detail
{
    struct WorldSectionTransactionAccess final
    {
        static void createEntities(
            WorldEdit& edit,
            std::span<Entity> entities
        )
        {
            require(edit.world_ != nullptr);
            edit.world_->registry_.create(entities.begin(), entities.end());
        }

        static void destroyEntities(
            WorldEdit& edit,
            std::span<const Entity> entities
        )
        {
            require(edit.world_ != nullptr);
            edit.world_->registry_.destroy(entities.begin(), entities.end());
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
    };
} // namespace lux::ecs::detail

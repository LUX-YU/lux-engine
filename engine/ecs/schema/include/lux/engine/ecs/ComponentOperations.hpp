#pragma once

#include <lux/engine/ecs/World.hpp>

#include <entt/core/type_info.hpp>

#include <cstddef>
#include <memory>
#include <type_traits>

namespace lux::ecs
{
    struct ComponentOperations final
    {
        std::uint64_t storage_id{};
        bool (*has)(const World&, Entity) noexcept{};
        void* (*get)(World&, Entity) noexcept{};
        const void* (*get_const)(const World&, Entity) noexcept{};
        void (*erase)(WorldEdit&, Entity) noexcept{};
        void (*reserve)(WorldEdit&, std::size_t){};
        void (*clone)(const World&, Entity, WorldEdit&, Entity){};
        void* (*default_emplace)(WorldEdit&, Entity){};
    };

    template <class Component>
    [[nodiscard]] ComponentOperations componentOperations() noexcept
    {
        ComponentOperations result;
        result.storage_id = entt::type_hash<Component>::value();
        result.has = [](const World& world, Entity entity) noexcept
        {
            return world.find<Component>(entity) != nullptr;
        };
        result.get = [](World& world, Entity entity) noexcept -> void*
        {
            return const_cast<Component*>(world.find<Component>(entity));
        };
        result.get_const = [](const World& world, Entity entity) noexcept -> const void*
        {
            return world.find<Component>(entity);
        };
        result.erase = [](WorldEdit& edit, Entity entity) noexcept
        {
            edit.erase<Component>(entity);
        };
        result.reserve = [](WorldEdit& edit, std::size_t count)
        {
            edit.reserve<Component>(count);
        };
        if constexpr (std::is_copy_constructible_v<Component>)
        {
            result.clone = [](
                const World& source,
                Entity source_entity,
                WorldEdit& target,
                Entity target_entity)
            {
                const Component* value = source.find<Component>(source_entity);
                if (value != nullptr)
                    target.emplace<Component>(target_entity, *value);
            };
        }
        if constexpr (std::is_default_constructible_v<Component>)
        {
            result.default_emplace = [](WorldEdit& edit, Entity entity) -> void*
            {
                return std::addressof(edit.emplace<Component>(entity));
            };
        }
        return result;
    }
} // namespace lux::ecs

#pragma once

#include <lux/engine/ecs/World.hpp>

#include <entt/core/type_info.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace lux::ecs
{
    namespace detail
    {
        struct ComponentOperationsAccess;
    }

    class ComponentOperations final
    {
      public:
        ComponentOperations() noexcept = default;

        [[nodiscard]] bool valid() const noexcept
        {
            return has_ != nullptr && get_ != nullptr && size_ != nullptr &&
                erase_ != nullptr && reserve_ != nullptr;
        }

        [[nodiscard]] bool has(
            const World& world,
            Entity entity
        ) const noexcept
        {
            detail::require(has_ != nullptr);
            return has_(world, entity);
        }

        [[nodiscard]] const void* get(
            const World& world,
            Entity entity
        ) const noexcept
        {
            detail::require(get_ != nullptr);
            return get_(world, entity);
        }

        [[nodiscard]] std::size_t size(const World& world) const noexcept
        {
            detail::require(size_ != nullptr);
            return size_(world);
        }

        void erase(WorldEdit& edit, Entity entity) const noexcept
        {
            detail::require(erase_ != nullptr);
            erase_(edit, entity);
        }

        void reserve(WorldEdit& edit, std::size_t count) const
        {
            detail::require(reserve_ != nullptr);
            reserve_(edit, count);
        }

        [[nodiscard]] bool cloneable() const noexcept
        {
            return clone_ != nullptr;
        }

        void clone(
            const World& source,
            Entity source_entity,
            WorldEdit& target,
            Entity target_entity
        ) const
        {
            detail::require(clone_ != nullptr);
            clone_(source, source_entity, target, target_entity);
        }

      private:
        using HasFn = bool (*)(const World&, Entity) noexcept;
        using GetFn = const void* (*)(const World&, Entity) noexcept;
        using SizeFn = std::size_t (*)(const World&) noexcept;
        using EraseFn = void (*)(WorldEdit&, Entity) noexcept;
        using ReserveFn = void (*)(WorldEdit&, std::size_t);
        using CloneFn = void (*)(const World&, Entity, WorldEdit&, Entity);
        using DefaultEmplaceFn = void* (*)(WorldEdit&, Entity);

        std::uint64_t storage_key_{};
        HasFn has_{};
        GetFn get_{};
        SizeFn size_{};
        EraseFn erase_{};
        ReserveFn reserve_{};
        CloneFn clone_{};
        DefaultEmplaceFn default_emplace_{};

        friend struct detail::ComponentOperationsAccess;

        template <class Component>
        friend ComponentOperations componentOperations() noexcept;
    };

    template <class Component>
    [[nodiscard]] ComponentOperations componentOperations() noexcept
    {
        ComponentOperations result;
        result.storage_key_ = entt::type_hash<Component>::value();
        result.has_ = [](const World& world, Entity entity) noexcept
        {
            return world.find<Component>(entity) != nullptr;
        };
        result.get_ = [](const World& world, Entity entity) noexcept -> const void*
        {
            return world.find<Component>(entity);
        };
        result.size_ = [](const World& world) noexcept
        {
            const auto* storage =
                world.registry_.template storage<Component>();
            return storage == nullptr ? 0U : storage->size();
        };
        result.erase_ = [](WorldEdit& edit, Entity entity) noexcept
        {
            edit.erase<Component>(entity);
        };
        result.reserve_ = [](WorldEdit& edit, std::size_t count)
        {
            edit.reserve<Component>(count);
        };
        if constexpr (std::is_copy_constructible_v<Component>)
        {
            result.clone_ = [](
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
            result.default_emplace_ = [](
                WorldEdit& edit,
                Entity entity
            ) -> void*
            {
                return std::addressof(edit.emplace<Component>(entity));
            };
        }
        return result;
    }
} // namespace lux::ecs

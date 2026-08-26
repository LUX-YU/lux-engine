#pragma once

#include <lux/engine/ecs/EcsState.hpp>

#include <entt/core/type_info.hpp>

#include <cstddef>
#include <cstdint>

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
            const EcsState& world,
            Entity entity
        ) const noexcept
        {
            detail::require(has_ != nullptr);
            return has_(world, entity);
        }

        [[nodiscard]] const void* get(
            const EcsState& world,
            Entity entity
        ) const noexcept
        {
            detail::require(get_ != nullptr);
            return get_(world, entity);
        }

        [[nodiscard]] std::size_t size(const EcsState& world) const noexcept
        {
            detail::require(size_ != nullptr);
            return size_(world);
        }

        void erase(EcsMutation& edit, Entity entity) const noexcept
        {
            detail::require(erase_ != nullptr);
            erase_(edit, entity);
        }

        void reserve(EcsMutation& edit, std::size_t count) const
        {
            detail::require(reserve_ != nullptr);
            reserve_(edit, count);
        }

      private:
        using HasFn = bool (*)(const EcsState&, Entity) noexcept;
        using GetFn = const void* (*)(const EcsState&, Entity) noexcept;
        using SizeFn = std::size_t (*)(const EcsState&) noexcept;
        using EraseFn = void (*)(EcsMutation&, Entity) noexcept;
        using ReserveFn = void (*)(EcsMutation&, std::size_t);

        std::uint64_t storage_key_{};
        HasFn has_{};
        GetFn get_{};
        SizeFn size_{};
        EraseFn erase_{};
        ReserveFn reserve_{};

        friend struct detail::ComponentOperationsAccess;

        template <class Component>
        friend ComponentOperations componentOperations() noexcept;
    };

    template <class Component>
    [[nodiscard]] ComponentOperations componentOperations() noexcept
    {
        ComponentOperations result;
        result.storage_key_ = entt::type_hash<Component>::value();
        result.has_ = [](const EcsState& world, Entity entity) noexcept
        {
            return world.find<Component>(entity) != nullptr;
        };
        result.get_ = [](const EcsState& world, Entity entity) noexcept -> const void*
        {
            return world.find<Component>(entity);
        };
        result.size_ = [](const EcsState& world) noexcept
        {
            const auto* storage =
                world.registry_.template storage<Component>();
            return storage == nullptr ? 0U : storage->size();
        };
        result.erase_ = [](EcsMutation& edit, Entity entity) noexcept
        {
            edit.erase<Component>(entity);
        };
        result.reserve_ = [](EcsMutation& edit, std::size_t count)
        {
            edit.reserve<Component>(count);
        };
        return result;
    }
} // namespace lux::ecs

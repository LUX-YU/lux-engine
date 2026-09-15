#pragma once

#include <lux/engine/simulation/ecs/Registry.hpp>

#include <entt/core/type_info.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <type_traits>
#include <utility>

namespace lux::simulation::ecs
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
        return has_ != nullptr && get_ != nullptr && size_ != nullptr && erase_ != nullptr && reserve_ != nullptr;
    }

    [[nodiscard]] bool has(const Registry &registry, Entity entity) const noexcept
    {
        if (has_ == nullptr)
        {
            std::terminate();
        }
        return has_(registry, entity);
    }

    [[nodiscard]] const void *get(const Registry &registry, Entity entity) const noexcept
    {
        if (get_ == nullptr)
        {
            std::terminate();
        }
        return get_(registry, entity);
    }

    [[nodiscard]] std::size_t size(const Registry &registry) const noexcept
    {
        if (size_ == nullptr)
        {
            std::terminate();
        }
        return size_(registry);
    }

    void erase(Registry &registry, Entity entity) const noexcept
    {
        if (erase_ == nullptr)
        {
            std::terminate();
        }
        erase_(registry, entity);
    }

    void reserve(Registry &registry, std::size_t count) const
    {
        if (reserve_ == nullptr)
        {
            std::terminate();
        }
        reserve_(registry, count);
    }

    // Publish an already committed value through the same update channel as a
    // typed patch.
    void notifyUpdated(Registry &registry, Entity entity) const noexcept
    {
        if (notify_updated_ == nullptr)
        {
            std::terminate();
        }
        notify_updated_(registry, entity);
    }

    [[nodiscard]] bool canTransfer() const noexcept
    {
        return transfer_ != nullptr;
    }

    // The caller has prepared the value and its final Entity references in a
    // private Registry. Destination identity and component absence are commit
    // preconditions, not recoverable errors.
    void transfer(Registry &destination, Registry &prepared, Entity entity) const noexcept
    {
        if (!transfer_)
        {
            std::terminate();
        }
        transfer_(destination, prepared, entity);
    }

  private:
    using HasFn = bool (*)(const Registry &, Entity) noexcept;
    using GetFn = const void *(*)(const Registry &, Entity) noexcept;
    using SizeFn = std::size_t (*)(const Registry &) noexcept;
    using EraseFn = void (*)(Registry &, Entity) noexcept;
    using ReserveFn = void (*)(Registry &, std::size_t);
    using NotifyUpdatedFn = void (*)(Registry &, Entity) noexcept;
    using TransferFn = void (*)(Registry &, Registry &, Entity) noexcept;

    std::uint64_t storage_key_{};
    HasFn has_{};
    GetFn get_{};
    SizeFn size_{};
    EraseFn erase_{};
    ReserveFn reserve_{};
    NotifyUpdatedFn notify_updated_{};
    TransferFn transfer_{};

    friend struct detail::ComponentOperationsAccess;

    template <class Component> friend ComponentOperations componentOperations() noexcept;
};

template <class Component> [[nodiscard]] ComponentOperations componentOperations() noexcept
{
    ComponentOperations result;
    result.storage_key_ = entt::type_hash<Component>::value();
    result.has_ = [](const Registry &registry, Entity entity) noexcept
    {
        return registry.template all_of<Component>(entity);
    };
    result.get_ = [](const Registry &registry, Entity entity) noexcept -> const void *
    {
        return registry.template try_get<Component>(entity);
    };
    result.size_ = [](const Registry &registry) noexcept
    {
        const auto *storage = registry.template storage<Component>();
        return storage == nullptr ? 0U : storage->size();
    };
    result.erase_ = [](Registry &registry, Entity entity) noexcept
    {
        registry.template remove<Component>(entity);
    };
    result.reserve_ = [](Registry &registry, std::size_t count)
    {
        registry.template storage<Component>().reserve(count);
    };
    result.notify_updated_ = [](Registry &registry, Entity entity) noexcept
    {
        registry.template patch<Component>(entity);
    };
    if constexpr (std::is_move_constructible_v<Component>)
    {
        result.transfer_ = [](Registry &destination, Registry &prepared, Entity entity) noexcept
        {
                if constexpr (std::is_void_v<decltype(prepared.template get<Component>(entity))>)
            {
                destination.template emplace<Component>(entity);
            }
            else
            {
                destination.template emplace<Component>(entity, std::move(prepared.template get<Component>(entity)));
            }
        };
    }
    return result;
}
} // namespace lux::simulation::ecs

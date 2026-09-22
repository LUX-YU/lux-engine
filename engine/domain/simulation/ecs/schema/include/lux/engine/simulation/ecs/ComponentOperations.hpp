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
    using MembershipChanges = std::remove_reference_t<decltype(std::declval<Registry &>().storage<entt::reactive>())>;
    ComponentOperations() noexcept = default;

    [[nodiscard]] std::size_t valueBytes() const noexcept
    {
        return value_bytes_;
    }

    // Register the typed membership signals in an existing reactive storage.
    // The storage owner disconnects before the schema's code lease is released.
    void trackMembership(MembershipChanges &changes) const
    {
        track_membership_(changes);
    }

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

  private:
    using HasFn = bool (*)(const Registry &, Entity) noexcept;
    using GetFn = const void *(*)(const Registry &, Entity) noexcept;
    using SizeFn = std::size_t (*)(const Registry &) noexcept;
    using EraseFn = void (*)(Registry &, Entity) noexcept;
    using ReserveFn = void (*)(Registry &, std::size_t);
    using NotifyUpdatedFn = void (*)(Registry &, Entity) noexcept;

    std::uint64_t storage_key_{};
    std::size_t value_bytes_{};
    HasFn has_{};
    GetFn get_{};
    SizeFn size_{};
    EraseFn erase_{};
    ReserveFn reserve_{};
    NotifyUpdatedFn notify_updated_{};
    void (*track_membership_)(MembershipChanges &){};

    friend struct detail::ComponentOperationsAccess;

    template <class Component> friend ComponentOperations componentOperations() noexcept;
};

template <class Component> [[nodiscard]] ComponentOperations componentOperations() noexcept
{
    ComponentOperations result;
    result.storage_key_ = entt::type_hash<Component>::value();
    result.value_bytes_ = sizeof(Component);
    result.track_membership_ = [](ComponentOperations::MembershipChanges &changes) {
        changes.template on_construct<Component>().template on_destroy<Component>();
    };
    result.has_ = [](const Registry &registry, Entity entity) noexcept {
        return registry.template all_of<Component>(entity);
    };
    result.get_ = [](const Registry &registry, Entity entity) noexcept -> const void * {
        return registry.template try_get<Component>(entity);
    };
    result.size_ = [](const Registry &registry) noexcept {
        const auto *storage = registry.template storage<Component>();
        return storage == nullptr ? 0U : storage->size();
    };
    result.erase_ = [](Registry &registry, Entity entity) noexcept { registry.template remove<Component>(entity); };
    result.reserve_ = [](Registry &registry, std::size_t count) {
        registry.template storage<Component>().reserve(count);
    };
    result.notify_updated_ = [](Registry &registry, Entity entity) noexcept {
        registry.template patch<Component>(entity);
    };
    return result;
}
} // namespace lux::simulation::ecs

#pragma once
/**
 * @file World.hpp
 * @brief Domain-neutral ECS data owner.
 *
 * World owns only the entity/component registry. System ownership, topology,
 * mutation and ticking live in Schedule. Keeping the two objects separate
 * makes their lifetime edge explicit: construct World first, then Schedule;
 * destroy Schedule first, then World.
 */

#include <lux/engine/ecs/Registry.hpp>

#include <utility>

namespace lux::ecs
{
    class World final
    {
    public:
        World() = default;
        World(const World&)            = delete;
        World& operator=(const World&) = delete;
        World(World&&)                 = delete;
        World& operator=(World&&)      = delete;

        [[nodiscard]] lux::ecs::Entity createEntity()
        {
            return registry_.create();
        }

        void destroyEntity(lux::ecs::Entity id)
        {
            if (registry_.valid(id)) registry_.destroy(id);
        }

        [[nodiscard]] bool valid(lux::ecs::Entity id) const noexcept
        {
            return registry_.valid(id);
        }

        template <class Component, class... Args>
        decltype(auto) emplace(lux::ecs::Entity id, Args&&... args)
        {
            return registry_.emplace<Component>(
                id, std::forward<Args>(args)...);
        }

        template <class Component>
        [[nodiscard]] Component& get(lux::ecs::Entity id)
        {
            return registry_.get<Component>(id);
        }

        template <class Component>
        [[nodiscard]] const Component& get(lux::ecs::Entity id) const
        {
            return registry_.get<Component>(id);
        }

        template <class Component>
        [[nodiscard]] Component* tryGet(lux::ecs::Entity id)
        {
            return registry_.try_get<Component>(id);
        }

        template <class Component>
        [[nodiscard]] const Component* tryGet(lux::ecs::Entity id) const
        {
            return registry_.try_get<Component>(id);
        }

        template <class Component>
        [[nodiscard]] bool has(lux::ecs::Entity id) const
        {
            return registry_.all_of<Component>(id);
        }

        template <class Component>
        void remove(lux::ecs::Entity id)
        {
            registry_.remove<Component>(id);
        }

        [[nodiscard]] lux::ecs::Registry& registry() noexcept
        {
            return registry_;
        }

        [[nodiscard]] const lux::ecs::Registry& registry() const noexcept
        {
            return registry_;
        }

    private:
        lux::ecs::Registry registry_;
    };

} // namespace lux::ecs

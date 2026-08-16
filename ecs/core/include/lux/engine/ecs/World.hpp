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

#include <lux/engine/meta/LuxObject.hpp>

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

        [[nodiscard]] lux::meta::entity_id createEntity()
        {
            return registry_.create();
        }

        void destroyEntity(lux::meta::entity_id id)
        {
            if (registry_.valid(id)) registry_.destroy(id);
        }

        [[nodiscard]] bool valid(lux::meta::entity_id id) const noexcept
        {
            return registry_.valid(id);
        }

        template <class Component, class... Args>
        decltype(auto) emplace(lux::meta::entity_id id, Args&&... args)
        {
            return registry_.emplace<Component>(
                id, std::forward<Args>(args)...);
        }

        template <class Component>
        [[nodiscard]] Component& get(lux::meta::entity_id id)
        {
            return registry_.get<Component>(id);
        }

        template <class Component>
        [[nodiscard]] const Component& get(lux::meta::entity_id id) const
        {
            return registry_.get<Component>(id);
        }

        template <class Component>
        [[nodiscard]] Component* tryGet(lux::meta::entity_id id)
        {
            return registry_.try_get<Component>(id);
        }

        template <class Component>
        [[nodiscard]] const Component* tryGet(lux::meta::entity_id id) const
        {
            return registry_.try_get<Component>(id);
        }

        template <class Component>
        [[nodiscard]] bool has(lux::meta::entity_id id) const
        {
            return registry_.all_of<Component>(id);
        }

        template <class Component>
        void remove(lux::meta::entity_id id)
        {
            registry_.remove<Component>(id);
        }

        [[nodiscard]] lux::meta::EntityRegistry& registry() noexcept
        {
            return registry_;
        }

        [[nodiscard]] const lux::meta::EntityRegistry& registry() const noexcept
        {
            return registry_;
        }

    private:
        lux::meta::EntityRegistry registry_;
    };

} // namespace lux::ecs

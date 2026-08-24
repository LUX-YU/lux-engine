#pragma once

#include <lux/engine/ecs/World.hpp>

namespace lux::ecs
{
    class SystemStart final
    {
      public:
        [[nodiscard]] bool boundTo(const World& world) const noexcept
        {
            return world_ == std::addressof(world);
        }

        [[nodiscard]] bool valid(Entity entity) const noexcept
        {
            return world_->valid(entity);
        }

        template <class Component>
        [[nodiscard]] const Component* find(Entity entity) const noexcept
        {
            return world_->template find<Component>(entity);
        }

        template <class Component>
        [[nodiscard]] const Component& get(Entity entity) const noexcept
        {
            return world_->template get<Component>(entity);
        }

        template <class... Access>
        [[nodiscard]] auto query() const
        {
            static_assert((!detail::AccessTraits<Access>::kWrite && ...));
            return world_->template query<Access...>();
        }

        template <class... Access>
        [[nodiscard]] auto query(QuerySpec<Access...> spec) const
        {
            return world_->query(spec);
        }

      private:
        explicit SystemStart(const World& world) noexcept : world_(&world) {}

        const World* world_{};

        friend class ScheduleEdit;
    };
} // namespace lux::ecs

#pragma once

#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/WorldCommands.hpp>

#include <entt/core/type_info.hpp>

#include <cstdint>
#include <type_traits>
#include <utility>

namespace lux::ecs
{
    class SystemFrame final
    {
      public:
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
            return detail::BasicQuery<World::Registry, Access...>(
                world_->registry_,
                detail::worldChangeRecorder(*world_)
            );
        }

        template <class... Access>
        [[nodiscard]] auto query(QuerySpec<Access...>) const
        {
            return query<Access...>();
        }

        template <class Component, class Fn>
            requires std::is_nothrow_invocable_v<Fn, Component&>
        void update(Entity entity, Fn&& fn) const noexcept
        {
            detail::require(
                world_->valid(entity) &&
                world_->registry_.template all_of<Component>(entity)
            );
            world_->registry_.template patch<Component>(
                entity,
                std::forward<Fn>(fn)
            );
            detail::recordWorldComponentChange(
                *world_,
                entt::type_hash<Component>::value(),
                entity,
                EComponentChangeKind::MODIFIED
            );
        }

        template <class Component>
        [[nodiscard]] ComponentChanges<Component> changes(
            ChangeCursor<Component>& cursor
        ) const noexcept
        {
            auto data = detail::readWorldComponentChanges(
                *world_,
                entt::type_hash<Component>::value(),
                detail::ChangeCursorAccess::epoch(cursor),
                detail::ChangeCursorAccess::sequence(cursor)
            );
            return ComponentChanges<Component>::fromDetail(data);
        }

        [[nodiscard]] EntityChanges entityChanges(
            EntityChangeCursor& cursor
        ) const noexcept
        {
            auto data = detail::readWorldEntityChanges(
                *world_,
                detail::ChangeCursorAccess::epoch(cursor),
                detail::ChangeCursorAccess::sequence(cursor)
            );
            return EntityChanges::fromDetail(data);
        }

        [[nodiscard]] WorldCommands commands() const noexcept
        {
            return commands_;
        }

        [[nodiscard]] float deltaSeconds() const noexcept
        {
            return delta_seconds_;
        }

        [[nodiscard]] std::uint64_t tickIndex() const noexcept
        {
            return tick_index_;
        }

      private:
        SystemFrame(
            World& world,
            WorldCommands commands,
            float delta_seconds,
            std::uint64_t tick_index
        ) noexcept
            : world_(&world),
              commands_(commands),
              delta_seconds_(delta_seconds),
              tick_index_(tick_index)
        {
        }

        World* world_{};
        WorldCommands commands_{};
        float delta_seconds_{};
        std::uint64_t tick_index_{};

        friend class Schedule;
    };
} // namespace lux::ecs

#pragma once

#include <lux/engine/ecs/ChangeSet.hpp>
#include <lux/engine/ecs/SystemAccessSpec.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/WorldCommands.hpp>

#include <entt/core/type_info.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace lux::ecs
{
    namespace detail
    {
        struct SystemContextAccess;
    }

    class SystemContext final
    {
    public:
        template <class Component>
        class Writer final
        {
        public:
            Writer(Writer&&) noexcept = default;
            Writer& operator=(Writer&&) noexcept = default;

            Writer(const Writer&) = delete;
            Writer& operator=(const Writer&) = delete;

            template <class Fn>
                requires std::is_nothrow_invocable_v<Fn, Component&>
            void update(Entity entity, Fn&& fn) noexcept
            {
                context_->template updateBound<Component>(
                    entity,
                    stream_,
                    std::forward<Fn>(fn)
                );
            }

        private:
            Writer(
                SystemContext& context,
                detail::BoundWorldChangeStream stream
            ) noexcept
                : context_(std::addressof(context)), stream_(stream)
            {
            }

            SystemContext* context_{};
            detail::BoundWorldChangeStream stream_{};

            friend class SystemContext;
        };

        [[nodiscard]] bool valid(Entity entity) const noexcept
        {
            return world_->valid(entity);
        }

        template <class Component>
        [[nodiscard]] const Component* find(Entity entity) const noexcept
        {
            validate<Component>(ESystemAccessMode::READ);
            return world_->template find<Component>(entity);
        }

        template <class Component>
        [[nodiscard]] const Component& get(Entity entity) const noexcept
        {
            validate<Component>(ESystemAccessMode::READ);
            return world_->template get<Component>(entity);
        }

        template <class... Access>
        [[nodiscard]] auto query()
        {
            (validate<typename detail::AccessTraits<Access>::ComponentType>(
                detail::AccessTraits<Access>::kWrite
                    ? ESystemAccessMode::WRITE
                    : ESystemAccessMode::READ
            ), ...);
            return detail::BasicQuery<World::Registry, Access...>(
                world_->registry_,
                changes_->binder()
            );
        }

        template <class... Access>
        [[nodiscard]] auto query(QuerySpec<Access...>)
        {
            return query<Access...>();
        }

        template <class Component>
        [[nodiscard]] Writer<Component> write() noexcept
        {
            validate<Component>(ESystemAccessMode::WRITE);
            return Writer<Component>(
                *this,
                changes_->binder()(entt::type_hash<Component>::value())
            );
        }

        template <class Component>
        [[nodiscard]] ComponentChanges<Component> changes(
            ChangeCursor<Component>& cursor
        ) const noexcept
        {
            validate<Component>(ESystemAccessMode::READ);
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
        SystemContext(
            World& world,
            ChangeSet& changes,
            WorldCommands commands,
            float delta_seconds,
            std::uint64_t tick_index,
            std::span<const SystemComponentAccess> allowed
        ) noexcept
            : world_(&world),
              changes_(&changes),
              commands_(commands),
              delta_seconds_(delta_seconds),
              tick_index_(tick_index),
              allowed_(allowed)
        {
        }

        template <class Component>
        void validate(ESystemAccessMode requested) const noexcept
        {
#if !defined(NDEBUG) || defined(LUX_ECS_CONTRACT_CHECKS)
            const auto type = lux::cxx::typeToken<Component>();
            for (const SystemComponentAccess& candidate : allowed_)
            {
                if (candidate.type == type &&
                    (candidate.mode == ESystemAccessMode::WRITE ||
                     requested == ESystemAccessMode::READ))
                {
                    return;
                }
            }
            detail::contractFailure();
#else
            (void)requested;
#endif
        }

        template <class Component, class Fn>
            requires std::is_nothrow_invocable_v<Fn, Component&>
        void updateBound(
            Entity entity,
            detail::BoundWorldChangeStream stream,
            Fn&& fn
        ) noexcept
        {
            detail::require(
                world_->valid(entity) &&
                world_->registry_.template all_of<Component>(entity)
            );
            world_->registry_.template patch<Component>(
                entity,
                std::forward<Fn>(fn)
            );
            stream(entity, EComponentChangeKind::MODIFIED);
        }

        World* world_{};
        ChangeSet* changes_{};
        WorldCommands commands_{};
        float delta_seconds_{};
        std::uint64_t tick_index_{};
        std::span<const SystemComponentAccess> allowed_;

        friend struct detail::SystemContextAccess;
    };
}

#pragma once

#include <lux/engine/ecs/Entity.hpp>
#include <lux/engine/ecs/core/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <entt/entity/registry.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace lux::ecs
{
    class Schedule;
    class SystemAttach;
    class WorldSnapshot;

    namespace persistence
    {
        class WorldSectionReader;
    }

    enum class EWorldEditError : std::uint8_t
    {
        NOT_IDLE,
        DESTROYING,
    };

    struct WorldEditError final
    {
        EWorldEditError code{EWorldEditError::NOT_IDLE};
    };

    namespace detail
    {
        struct WorldSnapshotAccess;
        struct WorldEditAccess;

        enum class EWorldState : std::uint8_t
        {
            IDLE,
            EDITING,
            EXECUTING,
            APPLYING_COMMANDS,
            DESTROYING,
        };

        [[noreturn]] LUX_ENGINE_ECS_CORE_PUBLIC void contractFailure() noexcept;

        inline void require(bool condition) noexcept
        {
            if (!condition)
                contractFailure();
        }
    } // namespace detail

    class World;

    class LUX_ENGINE_ECS_CORE_PUBLIC WorldEdit final
    {
      public:
        WorldEdit() noexcept = default;
        WorldEdit(const WorldEdit&) = delete;
        WorldEdit& operator=(const WorldEdit&) = delete;

        WorldEdit(WorldEdit&& other) noexcept;
        WorldEdit& operator=(WorldEdit&& other) noexcept;
        ~WorldEdit() noexcept;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return world_ != nullptr;
        }

        [[nodiscard]] Entity create();
        void destroy(Entity entity);

        template <class Component, class... Args>
        Component& emplace(Entity entity, Args&&... args);

        template <class Component>
        void remove(Entity entity);

        template <class Component>
        void reserve(std::size_t count);

      private:
        explicit WorldEdit(World& world, bool release_to_idle) noexcept;

        [[nodiscard]] Entity createAt(Entity entity);
        void release() noexcept;

        World* world_{};
        bool release_to_idle_{};

        friend class World;
        friend class Schedule;
        friend class WorldSnapshot;
        friend class persistence::WorldSectionReader;
        friend struct detail::WorldSnapshotAccess;
        friend struct detail::WorldEditAccess;
    };

    class LUX_ENGINE_ECS_CORE_PUBLIC World final
    {
      public:
        World();
        ~World() noexcept;

        World(const World&) = delete;
        World& operator=(const World&) = delete;
        World(World&&) = delete;
        World& operator=(World&&) = delete;

        [[nodiscard]] bool valid(Entity entity) const noexcept
        {
            return registry_.valid(entity);
        }

        template <class... Components>
        [[nodiscard]] auto view() noexcept
        {
            return registry_.template view<Components...>();
        }

        template <class... Components>
        [[nodiscard]] auto view() const noexcept
        {
            return registry_.template view<Components...>();
        }

        template <class Component>
        [[nodiscard]] Component* find(Entity entity) noexcept
        {
            return registry_.template try_get<Component>(entity);
        }

        template <class Component>
        [[nodiscard]] const Component* find(Entity entity) const noexcept
        {
            return registry_.template try_get<Component>(entity);
        }

        template <class Component>
        [[nodiscard]] Component& get(Entity entity) noexcept
        {
            detail::require(valid(entity));
            return registry_.template get<Component>(entity);
        }

        template <class Component>
        [[nodiscard]] const Component& get(Entity entity) const noexcept
        {
            detail::require(valid(entity));
            return registry_.template get<Component>(entity);
        }

        template <class Component, class Fn>
        void patch(Entity entity, Fn&& fn)
        {
            detail::require(valid(entity));
            registry_.template patch<Component>(
                entity,
                std::forward<Fn>(fn)
            );
        }

        [[nodiscard]] lux::cxx::expected<WorldEdit, WorldEditError>
        edit() noexcept;

      private:
        using Registry = entt::basic_registry<Entity>;

        template <class Component, auto Candidate, class Instance>
        [[nodiscard]] entt::connection connectConstruct(Instance& instance)
        {
            return registry_.template on_construct<Component>()
                .template connect<Candidate>(instance);
        }

        template <class Component, auto Candidate, class Instance>
        [[nodiscard]] entt::connection connectUpdate(Instance& instance)
        {
            return registry_.template on_update<Component>()
                .template connect<Candidate>(instance);
        }

        template <class Component, auto Candidate, class Instance>
        [[nodiscard]] entt::connection connectDestroy(Instance& instance)
        {
            return registry_.template on_destroy<Component>()
                .template connect<Candidate>(instance);
        }

        Registry registry_;
        detail::EWorldState state_{detail::EWorldState::IDLE};
        Schedule* schedule_{};
        std::size_t observer_relations_{};

        friend class WorldEdit;
        friend class Schedule;
        friend class SystemAttach;
        friend class WorldSnapshot;
        friend class persistence::WorldSectionReader;
        friend struct detail::WorldSnapshotAccess;
    };

    template <class Component, class... Args>
    Component& WorldEdit::emplace(Entity entity, Args&&... args)
    {
        detail::require(world_ != nullptr && world_->valid(entity));
        return world_->registry_.template emplace<Component>(
            entity,
            std::forward<Args>(args)...
        );
    }

    template <class Component>
    void WorldEdit::remove(Entity entity)
    {
        detail::require(world_ != nullptr && world_->valid(entity));
        world_->registry_.template remove<Component>(entity);
    }

    template <class Component>
    void WorldEdit::reserve(std::size_t count)
    {
        detail::require(world_ != nullptr);
        world_->registry_.template storage<Component>().reserve(count);
    }
} // namespace lux::ecs

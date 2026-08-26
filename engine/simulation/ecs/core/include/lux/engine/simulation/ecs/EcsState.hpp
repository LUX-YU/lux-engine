#pragma once

#include <lux/engine/simulation/ecs/Query.hpp>
#include <lux/engine/simulation/ecs/core/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <entt/entity/registry.hpp>

#include <cstddef>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <utility>

namespace lux::simulation::ecs
{
    class ComponentSnapshotBinding;
    class EcsChangeBatch;
    class SimulationEcsMutation;
    template <class Component>
    class TaskWriter;
    class EcsSnapshot;
    struct ComponentOperations;

    template <class Component>
    [[nodiscard]] ComponentOperations componentOperations() noexcept;

    enum class EEcsMutationError : std::uint8_t
    {
        NOT_IDLE,
        WRONG_THREAD,
        DESTROYING,
    };

    struct EcsMutationError final
    {
        EEcsMutationError code{EEcsMutationError::NOT_IDLE};
    };

    namespace detail
    {
        struct EcsSnapshotAccess;
        struct EcsMutationAccess;
        struct EcsColdAccess;
        struct EcsEntityAccess;

        enum class EEcsState : std::uint8_t
        {
            IDLE,
            MUTATING,
            DESTROYING,
        };

        [[noreturn]] LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC void contractFailure() noexcept;

        inline void require(bool condition) noexcept
        {
            if (!condition)
                contractFailure();
        }
    } // namespace detail

    class EcsState;

    class LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC EcsMutation final
    {
      public:
        EcsMutation() noexcept = default;
        EcsMutation(const EcsMutation&) = delete;
        EcsMutation& operator=(const EcsMutation&) = delete;
        EcsMutation(EcsMutation&& other) noexcept;
        EcsMutation& operator=(EcsMutation&& other) noexcept;
        ~EcsMutation() noexcept;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return world_ != nullptr;
        }

        [[nodiscard]] Entity create();
        void destroy(Entity entity);

        template <class Component, class... Args>
        Component& emplace(Entity entity, Args&&... args);

        template <class Component>
        void erase(Entity entity);

        template <class Component, class Fn>
            requires std::is_nothrow_invocable_v<Fn, Component&>
        void update(Entity entity, Fn&& fn) noexcept;

        template <class... Access>
        [[nodiscard]] auto query();

        template <class... Access>
        [[nodiscard]] auto query(QuerySpec<Access...>);

        template <class Component>
        void reserve(std::size_t count);

      private:
        explicit EcsMutation(
            EcsState& world,
            bool release_to_idle
        ) noexcept;
        [[nodiscard]] Entity createAt(Entity entity);
        void release() noexcept;

        EcsState* world_{};
        bool release_to_idle_{};

        friend class EcsState;
        friend class EcsSnapshot;
        friend struct detail::EcsSnapshotAccess;
        friend struct detail::EcsMutationAccess;
        friend struct detail::EcsColdAccess;
        friend class ComponentSnapshotBinding;
    };

    class LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC EcsState final
    {
      public:
        EcsState() noexcept;
        ~EcsState() noexcept;

        EcsState(const EcsState&) = delete;
        EcsState& operator=(const EcsState&) = delete;
        EcsState(EcsState&&) = delete;
        EcsState& operator=(EcsState&&) = delete;

        [[nodiscard]] bool valid(Entity entity) const noexcept
        {
            return registry_.valid(entity);
        }

        template <class Component>
        [[nodiscard]] const Component* find(Entity entity) const noexcept
        {
            return registry_.template try_get<Component>(entity);
        }

        template <class Component>
        [[nodiscard]] const Component& get(Entity entity) const noexcept
        {
            detail::require(valid(entity));
            return registry_.template get<Component>(entity);
        }

        template <class... Access>
        [[nodiscard]] auto query() const
        {
            static_assert((!detail::AccessTraits<Access>::kWrite && ...));
            return detail::BasicQuery<const Registry, Access...>(registry_);
        }

        template <class... Access>
        [[nodiscard]] auto query(QuerySpec<Access...>) const
        {
            return query<Access...>();
        }

        [[nodiscard]] lux::cxx::expected<EcsMutation, EcsMutationError>
        mutate() noexcept;

      private:
        using Registry = entt::basic_registry<Entity>;

        Registry registry_;
        std::thread::id owner_thread_;
        detail::EEcsState state_{detail::EEcsState::IDLE};

        friend class EcsMutation;
        friend class SimulationEcsMutation;
        friend class EcsSnapshot;
        template <class Component>
        friend class TaskWriter;
        template <class... Access>
        friend auto taskQuery(
            EcsState&,
            EcsChangeBatch&,
            QuerySpec<Access...>
        );
        template <class Component>
        friend TaskWriter<Component> taskWriter(
            EcsState&,
            EcsChangeBatch&
        ) noexcept;
        friend struct detail::EcsSnapshotAccess;
        friend struct detail::EcsColdAccess;
        friend struct detail::EcsEntityAccess;
        friend class ComponentSnapshotBinding;

        template <class Component>
        friend ComponentOperations componentOperations() noexcept;
    };

    namespace detail
    {
    } // namespace detail

    template <class Component, class... Args>
    Component& EcsMutation::emplace(Entity entity, Args&&... args)
    {
        detail::require(world_ != nullptr && world_->valid(entity));
        Component& result = world_->registry_.template emplace<Component>(
            entity,
            std::forward<Args>(args)...
        );
        return result;
    }

    template <class Component>
    void EcsMutation::erase(Entity entity)
    {
        detail::require(world_ != nullptr && world_->valid(entity));
        (void)world_->registry_.template remove<Component>(entity);
    }

    template <class Component, class Fn>
        requires std::is_nothrow_invocable_v<Fn, Component&>
    void EcsMutation::update(Entity entity, Fn&& fn) noexcept
    {
        detail::require(
            world_ != nullptr && world_->valid(entity) &&
            world_->registry_.template all_of<Component>(entity)
        );
        world_->registry_.template patch<Component>(
            entity,
            std::forward<Fn>(fn)
        );
    }

    template <class... Access>
    auto EcsMutation::query()
    {
        detail::require(world_ != nullptr);
        return detail::BasicQuery<EcsState::Registry, Access...>(
            world_->registry_,
            detail::ChangeStreamBinder{},
            {}
        );
    }

    template <class... Access>
    auto EcsMutation::query(QuerySpec<Access...>)
    {
        return query<Access...>();
    }

    template <class Component>
    void EcsMutation::reserve(std::size_t count)
    {
        detail::require(world_ != nullptr);
        world_->registry_.template storage<Component>().reserve(count);
    }
} // namespace lux::simulation::ecs

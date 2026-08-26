#pragma once

#include <lux/engine/simulation/ecs/EntityChanges.hpp>
#include <lux/engine/simulation/ecs/Query.hpp>
#include <lux/engine/simulation/ecs/core/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <entt/entity/registry.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

namespace lux::simulation::ecs
{
    class ComponentSnapshotBinding;
    class EcsChangeBatch;
    template <class Component>
    class TaskWriter;
    class EcsSnapshot;
    struct ComponentOperations;

    template <class Component>
    [[nodiscard]] ComponentOperations componentOperations() noexcept;

    struct EcsChangeHistoryBudget final
    {
        std::size_t initial_bytes;
        std::size_t max_bytes;
    };

    struct EcsStateConfig final
    {
        EcsChangeHistoryBudget changes;
    };

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
        class EcsChangeLog;
        struct EcsSnapshotAccess;
        struct EcsMutationAccess;
        struct EcsChangeAccess;
        struct EcsColdAccess;
        struct EcsEntityAccess;
        struct EcsExecutionAccess;

        enum class EEcsState : std::uint8_t
        {
            IDLE,
            MUTATING,
            EXECUTING,
            APPLYING_COMMANDS,
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

    enum class EEcsTaskExecutionError : std::uint8_t
    {
        WORLD_BUSY,
        WRONG_THREAD,
    };

    struct EcsTaskExecutionError final
    {
        EEcsTaskExecutionError code{EEcsTaskExecutionError::WORLD_BUSY};
    };

    /** Pure lexical EcsState-state lease. It never owns or runs a TaskGraph. */
    class LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC EcsTaskExecutionLease final
    {
      public:
        EcsTaskExecutionLease() noexcept = default;
        ~EcsTaskExecutionLease() noexcept;
        EcsTaskExecutionLease(EcsTaskExecutionLease&& other) noexcept;
        EcsTaskExecutionLease& operator=(
            EcsTaskExecutionLease&& other
        ) noexcept;
        EcsTaskExecutionLease(const EcsTaskExecutionLease&) = delete;
        EcsTaskExecutionLease& operator=(
            const EcsTaskExecutionLease&
        ) = delete;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return world_ != nullptr;
        }

      private:
        explicit EcsTaskExecutionLease(EcsState& world) noexcept;
        void release() noexcept;
        EcsState* world_{};

        friend class EcsState;
    };

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
        enum class EChangeEmission : std::uint8_t
        {
            RECORD,
            SUPPRESS,
        };

        explicit EcsMutation(
            EcsState& world,
            bool release_to_idle,
            EChangeEmission change_emission = EChangeEmission::RECORD
        ) noexcept;
        [[nodiscard]] Entity createAt(Entity entity);
        void release() noexcept;

        EcsState* world_{};
        bool release_to_idle_{};
        EChangeEmission change_emission_{EChangeEmission::RECORD};

        friend class EcsState;
        friend class EcsSnapshot;
        friend struct detail::EcsSnapshotAccess;
        friend struct detail::EcsMutationAccess;
        friend struct detail::EcsColdAccess;
        friend struct detail::EcsExecutionAccess;
        friend class ComponentSnapshotBinding;
    };

    class LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC EcsState final
    {
      public:
        explicit EcsState(EcsStateConfig config);
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

        [[nodiscard]] lux::cxx::expected<
            EcsTaskExecutionLease,
            EcsTaskExecutionError>
        beginTaskExecution() noexcept;

      private:
        using Registry = entt::basic_registry<Entity>;

        Registry registry_;
        EcsStateConfig config_;
        std::unique_ptr<detail::EcsChangeLog> changes_;
        std::thread::id owner_thread_;
        detail::EEcsState state_{detail::EEcsState::IDLE};
        bool execution_lease_{};

        friend class EcsMutation;
        friend class EcsTaskExecutionLease;
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
        friend struct detail::EcsChangeAccess;
        friend struct detail::EcsColdAccess;
        friend struct detail::EcsEntityAccess;
        friend struct detail::EcsExecutionAccess;
        friend class ComponentSnapshotBinding;

        template <class Component>
        friend ComponentOperations componentOperations() noexcept;
    };

    namespace detail
    {
        struct EcsChangeAccess final
        {
            [[nodiscard]] static EcsChangeLog& log(EcsState& world) noexcept
            {
                return *world.changes_;
            }

            [[nodiscard]] static const EcsChangeLog& log(
                const EcsState& world
            ) noexcept
            {
                return *world.changes_;
            }
        };

        [[nodiscard]] LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC ChangeRecorder
        ecsChangeRecorder(EcsState& world) noexcept;

        [[nodiscard]] LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC ChangeStreamBinder
        ecsChangeStreamBinder(EcsState& world) noexcept;

        [[nodiscard]] LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC bool recordEcsComponentChange(
            EcsState& world,
            std::uint64_t storage,
            Entity entity,
            EComponentChangeKind kind
        ) noexcept;

        [[nodiscard]] LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC bool recordEcsEntityChange(
            EcsState& world,
            Entity entity,
            EEntityChangeKind kind
        ) noexcept;

        LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC void establishEcsChangeBaseline(
            EcsState& world
        ) noexcept;

        LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC void markEcsChangeHistoryLoss(
            EcsState& world
        ) noexcept;

        [[nodiscard]] LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC std::uint64_t
        ecsChangeEpoch(const EcsState& world) noexcept;

        [[nodiscard]] LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC ChangeRangeData
        readEcsComponentChanges(
            const EcsState& world,
            std::uint64_t storage,
            std::uint64_t& cursor_epoch,
            std::uint64_t& cursor_sequence
        ) noexcept;

        [[nodiscard]] LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC ChangeRangeData
        readEcsEntityChanges(
            const EcsState& world,
            std::uint64_t& cursor_epoch,
            std::uint64_t& cursor_sequence
        ) noexcept;
    } // namespace detail

    template <class Component, class... Args>
    Component& EcsMutation::emplace(Entity entity, Args&&... args)
    {
        detail::require(world_ != nullptr && world_->valid(entity));
        const auto storage = entt::type_hash<Component>::value();
        Component& result = world_->registry_.template emplace<Component>(
            entity,
            std::forward<Args>(args)...
        );
        if (change_emission_ == EChangeEmission::RECORD)
        {
            (void)detail::recordEcsComponentChange(
                *world_, storage, entity,
                EComponentChangeKind::ADDED
            );
        }
        return result;
    }

    template <class Component>
    void EcsMutation::erase(Entity entity)
    {
        detail::require(world_ != nullptr && world_->valid(entity));
        const auto storage = entt::type_hash<Component>::value();
        if (world_->registry_.template remove<Component>(entity) != 0)
        {
            if (change_emission_ == EChangeEmission::RECORD)
            {
                (void)detail::recordEcsComponentChange(
                    *world_, storage, entity,
                    EComponentChangeKind::REMOVED
                );
            }
        }
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
        if (change_emission_ == EChangeEmission::RECORD)
        {
            (void)detail::recordEcsComponentChange(
                *world_, entt::type_hash<Component>::value(), entity,
                EComponentChangeKind::MODIFIED
            );
        }
    }

    template <class... Access>
    auto EcsMutation::query()
    {
        detail::require(world_ != nullptr);
        return detail::BasicQuery<EcsState::Registry, Access...>(
            world_->registry_,
            change_emission_ == EChangeEmission::RECORD
                ? detail::ecsChangeStreamBinder(*world_)
                : detail::ChangeStreamBinder{},
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

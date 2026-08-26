#pragma once

#include <lux/engine/ecs/EntityChanges.hpp>
#include <lux/engine/ecs/Query.hpp>
#include <lux/engine/ecs/core/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <entt/entity/registry.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

namespace lux::ecs
{
    class ComponentLoadBinding;
    class ComponentSnapshotBinding;
    class WorldChangeBatch;
    template <class Component>
    class TaskWriter;
    class WorldSnapshot;
    struct ComponentOperations;

    template <class Component>
    [[nodiscard]] ComponentOperations componentOperations() noexcept;

    struct WorldChangeHistoryBudget final
    {
        std::size_t initial_bytes;
        std::size_t max_bytes;
    };

    struct WorldConfig final
    {
        WorldChangeHistoryBudget changes;
    };

    enum class EWorldMutationError : std::uint8_t
    {
        NOT_IDLE,
        WRONG_THREAD,
        DESTROYING,
    };

    struct WorldMutationError final
    {
        EWorldMutationError code{EWorldMutationError::NOT_IDLE};
    };

    namespace detail
    {
        class WorldChangeLog;
        class SectionMembershipDirectory;
        struct WorldSnapshotAccess;
        struct WorldMutationAccess;
        struct WorldChangeAccess;
        struct WorldColdAccess;
        struct WorldEntityAccess;
        struct WorldExecutionAccess;
        struct PersistenceStorageAccess;
        struct WorldSectionTransactionAccess;
        struct WorldMembershipAccess;

        enum class EWorldState : std::uint8_t
        {
            IDLE,
            MUTATING,
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

    enum class EWorldTaskExecutionError : std::uint8_t
    {
        WORLD_BUSY,
        WRONG_THREAD,
    };

    struct WorldTaskExecutionError final
    {
        EWorldTaskExecutionError code{EWorldTaskExecutionError::WORLD_BUSY};
    };

    /** Pure lexical World-state lease. It never owns or runs a TaskGraph. */
    class LUX_ENGINE_ECS_CORE_PUBLIC WorldTaskExecutionLease final
    {
      public:
        WorldTaskExecutionLease() noexcept = default;
        ~WorldTaskExecutionLease() noexcept;
        WorldTaskExecutionLease(WorldTaskExecutionLease&& other) noexcept;
        WorldTaskExecutionLease& operator=(
            WorldTaskExecutionLease&& other
        ) noexcept;
        WorldTaskExecutionLease(const WorldTaskExecutionLease&) = delete;
        WorldTaskExecutionLease& operator=(
            const WorldTaskExecutionLease&
        ) = delete;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return world_ != nullptr;
        }

      private:
        explicit WorldTaskExecutionLease(World& world) noexcept;
        void release() noexcept;
        World* world_{};

        friend class World;
    };

    class LUX_ENGINE_ECS_CORE_PUBLIC WorldMutation final
    {
      public:
        WorldMutation() noexcept = default;
        WorldMutation(const WorldMutation&) = delete;
        WorldMutation& operator=(const WorldMutation&) = delete;
        WorldMutation(WorldMutation&& other) noexcept;
        WorldMutation& operator=(WorldMutation&& other) noexcept;
        ~WorldMutation() noexcept;

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

        explicit WorldMutation(
            World& world,
            bool release_to_idle,
            EChangeEmission change_emission = EChangeEmission::RECORD
        ) noexcept;
        [[nodiscard]] Entity createAt(Entity entity);
        void release() noexcept;

        World* world_{};
        bool release_to_idle_{};
        EChangeEmission change_emission_{EChangeEmission::RECORD};

        friend class World;
        friend class WorldSnapshot;
        friend struct detail::WorldSnapshotAccess;
        friend struct detail::WorldMutationAccess;
        friend struct detail::WorldColdAccess;
        friend struct detail::WorldExecutionAccess;
        friend class ComponentLoadBinding;
        friend class ComponentSnapshotBinding;
        friend struct detail::WorldSectionTransactionAccess;
    };

    class LUX_ENGINE_ECS_CORE_PUBLIC World final
    {
      public:
        explicit World(WorldConfig config);
        ~World() noexcept;

        World(const World&) = delete;
        World& operator=(const World&) = delete;
        World(World&&) = delete;
        World& operator=(World&&) = delete;

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

        [[nodiscard]] lux::cxx::expected<WorldMutation, WorldMutationError>
        mutate() noexcept;

        [[nodiscard]] lux::cxx::expected<
            WorldTaskExecutionLease,
            WorldTaskExecutionError>
        beginTaskExecution() noexcept;

      private:
        using Registry = entt::basic_registry<Entity>;

        Registry registry_;
        WorldConfig config_;
        std::unique_ptr<detail::WorldChangeLog> changes_;
        std::unique_ptr<detail::SectionMembershipDirectory>
            section_memberships_;
        std::thread::id owner_thread_;
        detail::EWorldState state_{detail::EWorldState::IDLE};
        bool execution_lease_{};
        std::uint64_t identity_{};
        std::size_t active_section_count_{};

        friend class WorldMutation;
        friend class WorldTaskExecutionLease;
        friend class WorldSnapshot;
        template <class Component>
        friend class TaskWriter;
        template <class... Access>
        friend auto taskQuery(
            World&,
            WorldChangeBatch&,
            QuerySpec<Access...>
        );
        template <class Component>
        friend TaskWriter<Component> taskWriter(
            World&,
            WorldChangeBatch&
        ) noexcept;
        friend struct detail::WorldSnapshotAccess;
        friend struct detail::WorldChangeAccess;
        friend struct detail::WorldColdAccess;
        friend struct detail::WorldEntityAccess;
        friend struct detail::WorldExecutionAccess;
        friend struct detail::PersistenceStorageAccess;
        friend class ComponentLoadBinding;
        friend class ComponentSnapshotBinding;
        friend struct detail::WorldSectionTransactionAccess;
        friend struct detail::WorldMembershipAccess;

        template <class Component>
        friend ComponentOperations componentOperations() noexcept;
    };

    namespace detail
    {
        struct WorldMembershipAccess final
        {
            [[nodiscard]] static LUX_ENGINE_ECS_CORE_PUBLIC std::uint32_t
            prepareAdd(
                World& world,
                Entity entity,
                std::uint64_t storage
            );

            static LUX_ENGINE_ECS_CORE_PUBLIC void commitAdd(
                World& world,
                Entity entity,
                std::uint32_t token
            ) noexcept;

            static LUX_ENGINE_ECS_CORE_PUBLIC void cancelAdd(
                World& world,
                std::uint32_t token
            ) noexcept;

            static LUX_ENGINE_ECS_CORE_PUBLIC void remove(
                World& world,
                Entity entity,
                std::uint64_t storage
            ) noexcept;
        };

        struct WorldChangeAccess final
        {
            [[nodiscard]] static WorldChangeLog& log(World& world) noexcept
            {
                return *world.changes_;
            }

            [[nodiscard]] static const WorldChangeLog& log(
                const World& world
            ) noexcept
            {
                return *world.changes_;
            }
        };

        [[nodiscard]] LUX_ENGINE_ECS_CORE_PUBLIC ChangeRecorder
        worldChangeRecorder(World& world) noexcept;

        [[nodiscard]] LUX_ENGINE_ECS_CORE_PUBLIC ChangeStreamBinder
        worldChangeStreamBinder(World& world) noexcept;

        [[nodiscard]] LUX_ENGINE_ECS_CORE_PUBLIC bool recordWorldComponentChange(
            World& world,
            std::uint64_t storage,
            Entity entity,
            EComponentChangeKind kind
        ) noexcept;

        [[nodiscard]] LUX_ENGINE_ECS_CORE_PUBLIC bool recordWorldEntityChange(
            World& world,
            Entity entity,
            EEntityChangeKind kind
        ) noexcept;

        LUX_ENGINE_ECS_CORE_PUBLIC void establishWorldChangeBaseline(
            World& world
        ) noexcept;

        LUX_ENGINE_ECS_CORE_PUBLIC void markWorldChangeHistoryLoss(
            World& world
        ) noexcept;

        [[nodiscard]] LUX_ENGINE_ECS_CORE_PUBLIC std::uint64_t
        worldChangeEpoch(const World& world) noexcept;

        [[nodiscard]] LUX_ENGINE_ECS_CORE_PUBLIC ChangeRangeData
        readWorldComponentChanges(
            const World& world,
            std::uint64_t storage,
            std::uint64_t& cursor_epoch,
            std::uint64_t& cursor_sequence
        ) noexcept;

        [[nodiscard]] LUX_ENGINE_ECS_CORE_PUBLIC ChangeRangeData
        readWorldEntityChanges(
            const World& world,
            std::uint64_t& cursor_epoch,
            std::uint64_t& cursor_sequence
        ) noexcept;
    } // namespace detail

    template <class Component, class... Args>
    Component& WorldMutation::emplace(Entity entity, Args&&... args)
    {
        detail::require(world_ != nullptr && world_->valid(entity));
        const auto storage = entt::type_hash<Component>::value();
        const std::uint32_t membership = detail::WorldMembershipAccess::prepareAdd(
            *world_,
            entity,
            storage
        );
        try
        {
            Component& result = world_->registry_.template emplace<Component>(
                entity,
                std::forward<Args>(args)...
            );
            detail::WorldMembershipAccess::commitAdd(
                *world_,
                entity,
                membership
            );
            if (change_emission_ == EChangeEmission::RECORD)
            {
                (void)detail::recordWorldComponentChange(
                    *world_, storage, entity,
                    EComponentChangeKind::ADDED
                );
            }
            return result;
        }
        catch (...)
        {
            detail::WorldMembershipAccess::cancelAdd(*world_, membership);
            throw;
        }
    }

    template <class Component>
    void WorldMutation::erase(Entity entity)
    {
        detail::require(world_ != nullptr && world_->valid(entity));
        const auto storage = entt::type_hash<Component>::value();
        if (world_->registry_.template remove<Component>(entity) != 0)
        {
            detail::WorldMembershipAccess::remove(
                *world_,
                entity,
                storage
            );
            if (change_emission_ == EChangeEmission::RECORD)
            {
                (void)detail::recordWorldComponentChange(
                    *world_, storage, entity,
                    EComponentChangeKind::REMOVED
                );
            }
        }
    }

    template <class Component, class Fn>
        requires std::is_nothrow_invocable_v<Fn, Component&>
    void WorldMutation::update(Entity entity, Fn&& fn) noexcept
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
            (void)detail::recordWorldComponentChange(
                *world_, entt::type_hash<Component>::value(), entity,
                EComponentChangeKind::MODIFIED
            );
        }
    }

    template <class... Access>
    auto WorldMutation::query()
    {
        detail::require(world_ != nullptr);
        return detail::BasicQuery<World::Registry, Access...>(
            world_->registry_,
            change_emission_ == EChangeEmission::RECORD
                ? detail::worldChangeStreamBinder(*world_)
                : detail::ChangeStreamBinder{},
            {}
        );
    }

    template <class... Access>
    auto WorldMutation::query(QuerySpec<Access...>)
    {
        return query<Access...>();
    }

    template <class Component>
    void WorldMutation::reserve(std::size_t count)
    {
        detail::require(world_ != nullptr);
        world_->registry_.template storage<Component>().reserve(count);
    }
} // namespace lux::ecs

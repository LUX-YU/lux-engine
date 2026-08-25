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
    class Schedule;
    class SystemFrame;
    class SystemStart;
    class WorldSnapshot;
    struct ComponentOperations;

    template <class Component>
    [[nodiscard]] ComponentOperations componentOperations() noexcept;

    struct ChangeJournalConfig final
    {
        std::size_t initial_bytes{256U * 1024U};
        std::size_t max_bytes{32U * 1024U * 1024U};
    };

    struct WorldConfig final
    {
        ChangeJournalConfig changes{};
    };

    enum class EWorldEditError : std::uint8_t
    {
        NOT_IDLE,
        WRONG_THREAD,
        DESTROYING,
    };

    struct WorldEditError final
    {
        EWorldEditError code{EWorldEditError::NOT_IDLE};
    };

    namespace detail
    {
        class ChangeJournal;
        struct WorldSnapshotAccess;
        struct WorldEditAccess;
        struct WorldChangeAccess;
        struct WorldColdAccess;
        struct WorldEntityAccess;
        struct PersistenceStorageAccess;

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

        explicit WorldEdit(
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
        friend class Schedule;
        friend class WorldSnapshot;
        friend struct detail::WorldSnapshotAccess;
        friend struct detail::WorldEditAccess;
        friend struct detail::WorldColdAccess;
    };

    class LUX_ENGINE_ECS_CORE_PUBLIC World final
    {
      public:
        explicit World(WorldConfig config = {});
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

        [[nodiscard]] lux::cxx::expected<WorldEdit, WorldEditError>
        edit() noexcept;

      private:
        using Registry = entt::basic_registry<Entity>;

        Registry registry_;
        WorldConfig config_;
        std::unique_ptr<detail::ChangeJournal> changes_;
        std::thread::id owner_thread_;
        detail::EWorldState state_{detail::EWorldState::IDLE};
        Schedule* schedule_{};

        friend class WorldEdit;
        friend class Schedule;
        friend class SystemFrame;
        friend class SystemStart;
        friend class WorldSnapshot;
        friend struct detail::WorldSnapshotAccess;
        friend struct detail::WorldChangeAccess;
        friend struct detail::WorldColdAccess;
        friend struct detail::WorldEntityAccess;
        friend struct detail::PersistenceStorageAccess;

        template <class Component>
        friend ComponentOperations componentOperations() noexcept;
    };

    namespace detail
    {
        struct WorldChangeAccess final
        {
            [[nodiscard]] static ChangeJournal& journal(World& world) noexcept
            {
                return *world.changes_;
            }

            [[nodiscard]] static const ChangeJournal& journal(
                const World& world
            ) noexcept
            {
                return *world.changes_;
            }
        };

        [[nodiscard]] LUX_ENGINE_ECS_CORE_PUBLIC ChangeRecorder
        worldChangeRecorder(World& world) noexcept;

        LUX_ENGINE_ECS_CORE_PUBLIC void recordWorldComponentChange(
            World& world,
            std::uint64_t storage,
            Entity entity,
            EComponentChangeKind kind
        ) noexcept;

        LUX_ENGINE_ECS_CORE_PUBLIC void recordWorldEntityChange(
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
    Component& WorldEdit::emplace(Entity entity, Args&&... args)
    {
        detail::require(world_ != nullptr && world_->valid(entity));
        Component& result = world_->registry_.template emplace<Component>(
            entity,
            std::forward<Args>(args)...
        );
        if (change_emission_ == EChangeEmission::RECORD)
        {
            detail::recordWorldComponentChange(
                *world_, entt::type_hash<Component>::value(), entity,
                EComponentChangeKind::ADDED
            );
        }
        return result;
    }

    template <class Component>
    void WorldEdit::erase(Entity entity)
    {
        detail::require(world_ != nullptr && world_->valid(entity));
        if (world_->registry_.template remove<Component>(entity) != 0 &&
            change_emission_ == EChangeEmission::RECORD)
        {
            detail::recordWorldComponentChange(
                *world_, entt::type_hash<Component>::value(), entity,
                EComponentChangeKind::REMOVED
            );
        }
    }

    template <class Component, class Fn>
        requires std::is_nothrow_invocable_v<Fn, Component&>
    void WorldEdit::update(Entity entity, Fn&& fn) noexcept
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
            detail::recordWorldComponentChange(
                *world_, entt::type_hash<Component>::value(), entity,
                EComponentChangeKind::MODIFIED
            );
        }
    }

    template <class... Access>
    auto WorldEdit::query()
    {
        detail::require(world_ != nullptr);
        return detail::BasicQuery<World::Registry, Access...>(
            world_->registry_,
            change_emission_ == EChangeEmission::RECORD
                ? detail::worldChangeRecorder(*world_)
                : detail::ChangeRecorder{}
        );
    }

    template <class... Access>
    auto WorldEdit::query(QuerySpec<Access...>)
    {
        return query<Access...>();
    }

    template <class Component>
    void WorldEdit::reserve(std::size_t count)
    {
        detail::require(world_ != nullptr);
        world_->registry_.template storage<Component>().reserve(count);
    }
} // namespace lux::ecs

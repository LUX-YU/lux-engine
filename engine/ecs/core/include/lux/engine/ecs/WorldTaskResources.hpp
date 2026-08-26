#pragma once

#include <lux/engine/ecs/Entity.hpp>
#include <lux/engine/ecs/Query.hpp>
#include <lux/engine/ecs/WorldCommands.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/core/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace lux::ecs
{
    class World;
    namespace detail
    {
        struct WorldTaskResourceTestAccess;
    }

    enum class EWorldTaskResourceError : std::uint8_t
    {
        INVALID_STORAGE,
        INVALID_PRODUCER,
        ALLOCATION_FAILURE,
        WORLD_BUSY,
    };

    struct WorldTaskResourceFailure final
    {
        EWorldTaskResourceError code{
            EWorldTaskResourceError::ALLOCATION_FAILURE
        };
    };

    struct WorldChangeBatchStats final
    {
        std::size_t current_records{};
        std::size_t peak_records{};
        std::size_t retained_capacity{};
        std::uint64_t lane_binds{};
        std::uint64_t journal_stream_binds{};
        std::uint64_t record_appends{};
        std::uint64_t per_record_lookups{};
        std::uint64_t history_losses{};
    };

    /**
     * Transient task-local canonical changes. Each component lane is resolved
     * once before row processing; publishing binds each World history stream
     * once and stops after the first loss of exact history.
     */
    class LUX_ENGINE_ECS_CORE_PUBLIC WorldChangeBatch final
    {
      public:
        WorldChangeBatch();
        ~WorldChangeBatch();
        WorldChangeBatch(WorldChangeBatch&&) noexcept;
        WorldChangeBatch& operator=(WorldChangeBatch&&) noexcept;
        WorldChangeBatch(const WorldChangeBatch&) = delete;
        WorldChangeBatch& operator=(const WorldChangeBatch&) = delete;

        [[nodiscard]] lux::cxx::expected<void, WorldTaskResourceFailure>
        prepare(
            std::span<const std::uint64_t> write_storages,
            std::size_t reserve_records = 0U
        ) noexcept;

        void reset() noexcept;
        [[nodiscard]] bool publish(World& world) noexcept;
        [[nodiscard]] WorldChangeBatchStats stats() const noexcept;

        /** Internal typed-query seam; callers bind once, never per record. */
        [[nodiscard]] detail::ChangeStreamBinder binder() noexcept;

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    class WorldCommandBatch;

    class LUX_ENGINE_ECS_CORE_PUBLIC WorldCommandRecordingScope final
    {
      public:
        WorldCommandRecordingScope() noexcept = default;
        ~WorldCommandRecordingScope() noexcept;
        WorldCommandRecordingScope(WorldCommandRecordingScope&&) noexcept;
        WorldCommandRecordingScope& operator=(
            WorldCommandRecordingScope&&
        ) noexcept = delete;
        WorldCommandRecordingScope(const WorldCommandRecordingScope&) = delete;
        WorldCommandRecordingScope& operator=(
            const WorldCommandRecordingScope&
        ) = delete;

        [[nodiscard]] WorldCommands commands() const noexcept;

      private:
        WorldCommandRecordingScope(
            WorldCommandBatch& owner,
            std::size_t producer,
            WorldCommands commands
        ) noexcept;

        WorldCommandBatch* owner_{};
        std::size_t producer_{};
        WorldCommands commands_{};

        friend class WorldCommandBatch;
    };

    /** Deterministic producer-ordered command resource for one graph run. */
    class LUX_ENGINE_ECS_CORE_PUBLIC WorldCommandBatch final
    {
      public:
        WorldCommandBatch();
        ~WorldCommandBatch();
        WorldCommandBatch(WorldCommandBatch&&) noexcept;
        WorldCommandBatch& operator=(WorldCommandBatch&&) noexcept;
        WorldCommandBatch(const WorldCommandBatch&) = delete;
        WorldCommandBatch& operator=(const WorldCommandBatch&) = delete;

        [[nodiscard]] lux::cxx::expected<void, WorldTaskResourceFailure>
        prepare(
            std::size_t producer_count,
            std::size_t reserve_commands_per_producer = 0U
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<
            WorldCommandRecordingScope,
            WorldTaskResourceFailure>
        begin(std::size_t producer) noexcept;

        [[nodiscard]] std::size_t discarded() const noexcept;
        [[nodiscard]] std::size_t allocationEvents() const noexcept;

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;

        void end(std::size_t producer) noexcept;
        friend class WorldCommandRecordingScope;
        friend struct detail::WorldTaskResourceTestAccess;
        friend LUX_ENGINE_ECS_CORE_PUBLIC void applyWorldCommands(
            World&,
            WorldCommandBatch&
        ) noexcept;
    };

    LUX_ENGINE_ECS_CORE_PUBLIC void applyWorldCommands(
        World& world,
        WorldCommandBatch& commands
    ) noexcept;

    /** One-bound-lane point writer for a graph task. */
    template <class Component>
    class TaskWriter final
    {
      public:
        template <class Fn>
            requires std::is_nothrow_invocable_v<Fn, Component&>
        void update(Entity entity, Fn&& fn) noexcept
        {
            detail::require(world_ != nullptr);
            detail::require(world_->state_ == detail::EWorldState::EXECUTING);
            detail::require(world_->valid(entity));
            world_->registry_.template patch<Component>(
                entity,
                std::forward<Fn>(fn)
            );
            if (!history_lost_)
                history_lost_ = !stream_(
                    entity,
                    EComponentChangeKind::MODIFIED
                );
        }

      private:
        TaskWriter(
            World& world,
            detail::BoundWorldChangeStream stream
        ) noexcept
            : world_(std::addressof(world)), stream_(stream)
        {
        }

        World* world_{};
        detail::BoundWorldChangeStream stream_{};
        bool history_lost_{};

        template <class Value>
        friend TaskWriter<Value> taskWriter(
            World&,
            WorldChangeBatch&
        ) noexcept;
    };

    template <class... Access>
    [[nodiscard]] auto taskQuery(
        World& world,
        WorldChangeBatch& changes,
        QuerySpec<Access...> specification
    )
    {
        detail::require(world.state_ == detail::EWorldState::EXECUTING);
        return detail::BasicQuery<World::Registry, Access...>(
            world.registry_,
            changes.binder()
        );
    }

    template <class Component>
    [[nodiscard]] TaskWriter<Component> taskWriter(
        World& world,
        WorldChangeBatch& changes
    ) noexcept
    {
        detail::require(world.state_ == detail::EWorldState::EXECUTING);
        return TaskWriter<Component>(
            world,
            changes.binder()(entt::type_hash<Component>::value())
        );
    }

    template <class Component>
    [[nodiscard]] ComponentChanges<Component> componentChanges(
        const World& world,
        ChangeCursor<Component>& cursor
    ) noexcept
    {
        auto data = detail::readWorldComponentChanges(
            world,
            entt::type_hash<Component>::value(),
            detail::ChangeCursorAccess::epoch(cursor),
            detail::ChangeCursorAccess::sequence(cursor)
        );
        return ComponentChanges<Component>::fromDetail(data);
    }

    [[nodiscard]] inline EntityChanges entityChanges(
        const World& world,
        EntityChangeCursor& cursor
    ) noexcept
    {
        auto data = detail::readWorldEntityChanges(
            world,
            detail::ChangeCursorAccess::epoch(cursor),
            detail::ChangeCursorAccess::sequence(cursor)
        );
        return EntityChanges::fromDetail(data);
    }
}

#pragma once

#include <lux/engine/simulation/ecs/Entity.hpp>
#include <lux/engine/simulation/ecs/Query.hpp>
#include <lux/engine/simulation/ecs/EcsChangeJournal.hpp>
#include <lux/engine/simulation/ecs/EcsCommands.hpp>
#include <lux/engine/simulation/ecs/EcsState.hpp>
#include <lux/engine/simulation/ecs/core/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace lux::simulation::ecs
{
    class EcsState;
    namespace detail
    {
        struct EcsTaskResourceTestAccess;
    }

    enum class EEcsTaskResourceError : std::uint8_t
    {
        INVALID_STORAGE,
        INVALID_PRODUCER,
        ALLOCATION_FAILURE,
        WORLD_BUSY,
    };

    struct EcsTaskResourceFailure final
    {
        EEcsTaskResourceError code{
            EEcsTaskResourceError::ALLOCATION_FAILURE
        };
    };

    struct EcsChangeBatchStats final
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
     * once before row processing; publishing binds each EcsState history stream
     * once and stops after the first loss of exact history.
     */
    class LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC EcsChangeBatch final
    {
      public:
        EcsChangeBatch();
        ~EcsChangeBatch();
        EcsChangeBatch(EcsChangeBatch&&) noexcept;
        EcsChangeBatch& operator=(EcsChangeBatch&&) noexcept;
        EcsChangeBatch(const EcsChangeBatch&) = delete;
        EcsChangeBatch& operator=(const EcsChangeBatch&) = delete;

        [[nodiscard]] lux::cxx::expected<void, EcsTaskResourceFailure>
        prepare(
            std::span<const std::uint64_t> write_storages,
            std::size_t reserve_records = 0U
        ) noexcept;

        void reset() noexcept;
        [[nodiscard]] bool publish(EcsChangeJournal& journal) noexcept;
        [[nodiscard]] EcsChangeBatchStats stats() const noexcept;

        /** Internal typed-query seam; callers bind once, never per record. */
        [[nodiscard]] detail::ChangeStreamBinder binder() noexcept;

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    class EcsCommandBatch;

    class LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC EcsCommandRecordingScope final
    {
      public:
        EcsCommandRecordingScope() noexcept = default;
        ~EcsCommandRecordingScope() noexcept;
        EcsCommandRecordingScope(EcsCommandRecordingScope&&) noexcept;
        EcsCommandRecordingScope& operator=(
            EcsCommandRecordingScope&&
        ) noexcept = delete;
        EcsCommandRecordingScope(const EcsCommandRecordingScope&) = delete;
        EcsCommandRecordingScope& operator=(
            const EcsCommandRecordingScope&
        ) = delete;

        [[nodiscard]] EcsCommands commands() const noexcept;

      private:
        EcsCommandRecordingScope(
            EcsCommandBatch& owner,
            std::size_t producer,
            EcsCommands commands
        ) noexcept;

        EcsCommandBatch* owner_{};
        std::size_t producer_{};
        EcsCommands commands_{};

        friend class EcsCommandBatch;
    };

    /** Deterministic producer-ordered command resource for one graph run. */
    class LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC EcsCommandBatch final
    {
      public:
        EcsCommandBatch();
        ~EcsCommandBatch();
        EcsCommandBatch(EcsCommandBatch&&) noexcept;
        EcsCommandBatch& operator=(EcsCommandBatch&&) noexcept;
        EcsCommandBatch(const EcsCommandBatch&) = delete;
        EcsCommandBatch& operator=(const EcsCommandBatch&) = delete;

        [[nodiscard]] lux::cxx::expected<void, EcsTaskResourceFailure>
        prepare(
            std::size_t producer_count,
            std::size_t reserve_commands_per_producer = 0U
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<
            EcsCommandRecordingScope,
            EcsTaskResourceFailure>
        begin(std::size_t producer) noexcept;

        [[nodiscard]] std::size_t discarded() const noexcept;
        [[nodiscard]] std::size_t allocationEvents() const noexcept;

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;

        void end(std::size_t producer) noexcept;
        friend class EcsCommandRecordingScope;
        friend struct detail::EcsTaskResourceTestAccess;
        friend LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC void applyEcsCommands(
            EcsState&,
            EcsChangeJournal&,
            EcsCommandBatch&
        ) noexcept;
    };

    LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC void applyEcsCommands(
        EcsState& world,
        EcsChangeJournal& journal,
        EcsCommandBatch& commands
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
            EcsState& world,
            detail::BoundEcsChangeStream stream
        ) noexcept
            : world_(std::addressof(world)), stream_(stream)
        {
        }

        EcsState* world_{};
        detail::BoundEcsChangeStream stream_{};
        bool history_lost_{};

        template <class Value>
        friend TaskWriter<Value> taskWriter(
            EcsState&,
            EcsChangeBatch&
        ) noexcept;
    };

    template <class... Access>
    [[nodiscard]] auto taskQuery(
        EcsState& world,
        EcsChangeBatch& changes,
        QuerySpec<Access...> specification
    )
    {
        return detail::BasicQuery<EcsState::Registry, Access...>(
            world.registry_,
            changes.binder()
        );
    }

    template <class Component>
    [[nodiscard]] TaskWriter<Component> taskWriter(
        EcsState& world,
        EcsChangeBatch& changes
    ) noexcept
    {
        return TaskWriter<Component>(
            world,
            changes.binder()(entt::type_hash<Component>::value())
        );
    }

    template <class Component>
    [[nodiscard]] ComponentChanges<Component> componentChanges(
        const EcsChangeJournal& journal,
        ChangeCursor<Component>& cursor
    ) noexcept
    {
        return journal.read(cursor);
    }

    [[nodiscard]] inline EntityChanges entityChanges(
        const EcsChangeJournal& journal,
        EntityChangeCursor& cursor
    ) noexcept
    {
        return journal.read(cursor);
    }
}

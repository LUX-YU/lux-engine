#pragma once

#include <lux/engine/ecs/Entity.hpp>
#include <lux/engine/ecs/Query.hpp>
#include <lux/engine/ecs/WorldCommands.hpp>
#include <lux/engine/ecs/core/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace lux::ecs
{
    class World;

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
        friend LUX_ENGINE_ECS_CORE_PUBLIC void applyWorldCommands(
            World&,
            WorldCommandBatch&
        ) noexcept;
    };

    LUX_ENGINE_ECS_CORE_PUBLIC void applyWorldCommands(
        World& world,
        WorldCommandBatch& commands
    ) noexcept;
}

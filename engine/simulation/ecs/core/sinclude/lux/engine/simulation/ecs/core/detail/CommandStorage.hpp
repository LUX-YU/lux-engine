#pragma once

#include <lux/engine/simulation/ecs/EcsCommands.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lux::simulation::ecs::detail
{
    struct CommandShardAccess;

    struct LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC CommandRecord final
    {
        void* payload{};
        void (*apply)(void*, SimulationEcsMutation&) noexcept{};
        void (*destroy)(void*) noexcept{};

        CommandRecord() noexcept = default;
        CommandRecord(const CommandRecord&) = delete;
        CommandRecord& operator=(const CommandRecord&) = delete;

        CommandRecord(CommandRecord&& other) noexcept;
        CommandRecord& operator=(CommandRecord&& other) noexcept;
        ~CommandRecord() noexcept;

        void reset() noexcept;
    };

    class CommandArena final
    {
      public:
        [[nodiscard]] void* allocate(
            std::size_t size,
            std::size_t alignment
        ) noexcept;
        void prepare(std::size_t bytes);
        void reset() noexcept;
        void swap(CommandArena& other) noexcept;
        [[nodiscard]] std::size_t allocationEvents() const noexcept;

      private:
        std::vector<std::byte> storage_;
        std::size_t used_{};
        std::size_t allocation_events_{};
    };

    class LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC CommandShard final
    {
      public:
        explicit CommandShard(
            std::uint32_t generation = 1,
            bool* batch_failed = nullptr
        ) noexcept;
        ~CommandShard() noexcept = default;

        CommandShard(const CommandShard&) = delete;
        CommandShard& operator=(const CommandShard&) = delete;
        CommandShard(CommandShard&& other) noexcept;
        CommandShard& operator=(CommandShard&& other) noexcept;

        void prepare(
            std::size_t command_capacity,
            std::size_t payload_capacity,
            bool& batch_failed
        );
        void invalidate() noexcept;

        [[nodiscard]] bool accepts(std::uint32_t generation) const noexcept;
        [[nodiscard]] std::uint32_t generation() const noexcept;
        [[nodiscard]] std::size_t discarded() const noexcept;
        [[nodiscard]] std::size_t allocationEvents() const noexcept;
        void failNextPushForTest() noexcept
        {
            fail_next_push_for_test_ = true;
        }

      private:
        [[nodiscard]] ECommandResult push(
            std::uint32_t writer_generation,
            const EcsCommands::CommandVTable& table,
            void* source
        ) noexcept;

        [[nodiscard]] EcsCommands beginExecution() noexcept;
        void endExecution() noexcept;
        void applyPending(SimulationEcsMutation& mutation) noexcept;

        std::vector<CommandRecord> pending_;
        CommandArena pending_arena_;
        std::uint32_t generation_{1};
        std::size_t discarded_{};
        std::size_t record_allocation_events_{};
        std::size_t max_commands_{};
        bool* batch_failed_{};
        bool active_{};
        bool applying_{};
        bool fail_next_push_for_test_{};

        friend class ::lux::simulation::ecs::EcsCommands;
        friend struct CommandShardAccess;
    };

    struct CommandShardAccess final
    {
        [[nodiscard]] static EcsCommands begin(CommandShard& shard) noexcept
        {
            return shard.beginExecution();
        }

        static void end(CommandShard& shard) noexcept
        {
            shard.endExecution();
        }

        static void apply(
            CommandShard& shard,
            SimulationEcsMutation& mutation
        ) noexcept
        {
            shard.applyPending(mutation);
        }
    };
} // namespace lux::simulation::ecs::detail

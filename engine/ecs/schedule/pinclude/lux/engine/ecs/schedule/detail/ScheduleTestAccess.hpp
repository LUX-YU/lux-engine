#pragma once

#include <lux/engine/ecs/Schedule.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lux::ecs::detail
{
    struct ExecutionPlanEntry final
    {
        lux::cxx::TypeToken type;
        std::uint32_t slot{};
        bool owner_thread_only{};
    };

    struct ExecutionPlanSnapshot final
    {
        std::vector<ExecutionPlanEntry> order;
        std::vector<std::vector<std::uint32_t>> batches;
    };

    struct LUX_ENGINE_ECS_SCHEDULE_PUBLIC ScheduleTestAccess final
    {
        [[nodiscard]] static ExecutionPlanSnapshot snapshot(
            const Schedule& schedule
        );

        [[nodiscard]] static std::size_t discardedCommands(
            const Schedule& schedule
        ) noexcept;

        [[nodiscard]] static std::size_t commandAllocationEvents(
            const Schedule& schedule
        ) noexcept;

        static void failNextCommandPush(
            Schedule& schedule,
            AnySystemHandle handle
        ) noexcept;
    };
} // namespace lux::ecs::detail

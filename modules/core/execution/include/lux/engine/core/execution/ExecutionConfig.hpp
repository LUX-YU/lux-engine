#pragma once

#include <cstddef>
#include <optional>

namespace lux::execution
{
    struct TimerQueueConfig final
    {
        std::size_t capacity{};
    };

    struct BlockingSchedulerConfig final
    {
        std::size_t concurrency{};
        std::size_t queue_capacity{};
    };

    struct ExecutionRuntimeConfig final
    {
        std::size_t cpu_concurrency{};
        std::size_t task_capacity{1024};
        TimerQueueConfig timer{};
        std::optional<BlockingSchedulerConfig> blocking;
        std::size_t task_history_capacity{256};
    };

} // namespace lux::execution

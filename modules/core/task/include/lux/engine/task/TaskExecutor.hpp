#pragma once

#include <lux/engine/task/TaskGraph.hpp>
#include <lux/engine/task/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lux::task
{
    /**
     * Number of persistent background workers is product policy and is therefore
     * supplied explicitly. worker_count == 0 is a supported debug/single-thread
     * mode: every task executes on the caller thread.
     *
     * initial_task_capacity is only a preallocation hint, not a hard capacity.
     */
    struct TaskExecutorConfig final
    {
        TaskExecutorConfig() = delete;

        constexpr TaskExecutorConfig(std::uint32_t workers, std::size_t task_capacity) noexcept
            : worker_count(workers), initial_task_capacity(task_capacity)
        {
        }

        std::uint32_t worker_count;
        std::size_t initial_task_capacity;
    };

    enum class ETaskExecutorError : std::uint8_t
    {
        ALREADY_EXECUTING,
        ALLOCATION_FAILURE,
    };

    struct TaskExecutorFailure final
    {
        ETaskExecutorError code{ETaskExecutorError::ALLOCATION_FAILURE};
    };

    /**
     * Persistent completion-driven worker executor.
     *
     * Hot-path dependency release is lock-free: execute callable, atomically
     * decrement successor prerequisite counters, publish newly-ready tasks.
     * Sleeping/wakeup is confined to idle paths.
     */
    class LUX_CORE_TASK_PUBLIC TaskExecutor final
    {
    public:
        explicit TaskExecutor(TaskExecutorConfig config);
        ~TaskExecutor() noexcept;

        TaskExecutor(TaskExecutor&&) noexcept;
        TaskExecutor& operator=(TaskExecutor&&) noexcept;

        TaskExecutor(const TaskExecutor&) = delete;
        TaskExecutor& operator=(const TaskExecutor&) = delete;

        [[nodiscard]] std::uint32_t workerCount() const noexcept;
        [[nodiscard]] std::size_t taskCapacity() const noexcept;

        /** Cold-path preallocation. execute() also grows automatically if needed. */
        [[nodiscard]] lux::cxx::expected<void, TaskExecutorFailure> reserve(std::size_t task_capacity) noexcept;

        /** Synchronous completion. The calling thread owns CALLER_THREAD tasks. */
        [[nodiscard]] lux::cxx::expected<void, TaskExecutorFailure> execute(const TaskGraph& graph) noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}

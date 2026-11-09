#pragma once

#include <lux/engine/core/execution/Scheduler.hpp>
#include <lux/engine/task/TaskGraph.hpp>
#include <lux/engine/task/visibility.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace lux::task
{
    enum class ETaskExecutorError : std::uint8_t
    {
        ALREADY_EXECUTING
    };

    class TaskGraphSender;

    // Executes one finite DAG at a time using a borrowed CPU scheduler and reusable storage.
    // The supplying Runtime, graph and captured objects must outlive terminal completion.
    class LUX_CORE_TASK_PUBLIC TaskExecutor final
    {
    public:
        explicit TaskExecutor(execution::CpuScheduler scheduler) noexcept;
        ~TaskExecutor() noexcept;
        TaskExecutor(const TaskExecutor&) = delete;
        TaskExecutor& operator=(const TaskExecutor&) = delete;

        // CALLER_THREAD nodes use the receiver environment's scheduler. It must enqueue
        // on that thread and remain available until this accepted graph has drained.
        // Cancellation is observed before admission, not halfway through a graph.
        [[nodiscard]] TaskGraphSender schedule(const TaskGraph& graph) noexcept;

    private:
        friend class TaskGraphSender;
        struct Impl;
        [[nodiscard]] bool acquire() noexcept;
        void release() noexcept;
        [[nodiscard]] execution::CpuScheduler cpuScheduler() const noexcept;
        [[nodiscard]] void* workspace(std::size_t count, std::size_t stride, std::size_t alignment) noexcept;
        std::unique_ptr<Impl> impl_;
    };

    // The connected operation owns node operation states; the Executor only lends storage.
    class TaskGraphSender final
    {
    public:
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(),
            stdexec::set_error_t(ETaskExecutorError),
            stdexec::set_stopped_t()
        >;

        template <class Receiver> class Operation;

        template <class Receiver>
        [[nodiscard]] Operation<std::decay_t<Receiver>> connect(Receiver&& receiver) const;

    private:
        friend class TaskExecutor;

        TaskGraphSender(TaskExecutor& executor, const TaskGraph& graph) noexcept : executor_(&executor), graph_(&graph)
        {
        }

        TaskExecutor* executor_;
        const TaskGraph* graph_;
    };
} // namespace lux::task

#include <lux/engine/task/detail/TaskGraphOperation.hpp>

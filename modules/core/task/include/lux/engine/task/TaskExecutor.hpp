#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/core/execution/Scheduler.hpp>
#include <lux/engine/task/TaskGraph.hpp>
#include <lux/engine/task/visibility.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <tuple>
#include <utility>

namespace lux::task
{
    enum class ETaskExecutorError : std::uint8_t
    {
        ALREADY_EXECUTING,
    };

    struct TaskExecutorFailure final
    {
        ETaskExecutorError code{ETaskExecutorError::ALREADY_EXECUTING};
    };

    class TaskGraphSender;

    /** Executes immutable DAGs on the application's CPU scheduler; owns no threads or ready queues.
     * One graph may use an executor at a time. Graph, callables and code pins outlive its completion.
     * Workspace is reused after completion. The supplying ExecutionRuntime outlives this executor.
     * Do not move the executor while a Sender or connected operation borrows it.
     */
    class LUX_CORE_TASK_PUBLIC TaskExecutor final
    {
    public:
        using Result = lux::cxx::expected<void, TaskExecutorFailure>;

        explicit TaskExecutor(execution::CpuScheduler cpu, std::size_t initial_task_capacity = 0) noexcept;
        ~TaskExecutor() noexcept;
        TaskExecutor(TaskExecutor&&) noexcept;
        TaskExecutor& operator=(TaskExecutor&&) noexcept;
        TaskExecutor(const TaskExecutor&) = delete;
        TaskExecutor& operator=(const TaskExecutor&) = delete;

        // A nondecreasing sizing hint, not allocated storage or a task limit. Actual workspace
        // is allocated on execution, once the connected scheduler/receiver state size is known.
        [[nodiscard]] std::size_t minimumTaskCapacity() const noexcept;
        [[nodiscard]] Result setMinimumTaskCapacity(std::size_t task_capacity) noexcept;

        /** CALLER_THREAD nodes run on the receiver's scheduler, which must enqueue on that thread, be infallible and
         * remain available until completion. Stop is observed before starting; an accepted finite DAG drains. This does
         * not authorize worker threads to access thread-affine ECS/Object state.
         */
        [[nodiscard]] TaskGraphSender schedule(const TaskGraph& graph) noexcept;

        /** Runs caller-affine nodes through sync_wait's local run_loop on this calling thread.
         * Does not pump application events or dispatch arbitrary Object/Editor work.
         */
        [[nodiscard]] Result execute(const TaskGraph& graph) noexcept;

    private:
        friend class TaskGraphSender;
        struct Impl;
        [[nodiscard]] bool acquire() noexcept;
        void release() noexcept;
        [[nodiscard]] execution::CpuScheduler cpu() const noexcept;
        [[nodiscard]] void* workspace(std::size_t count, std::size_t stride, std::size_t alignment) noexcept;
        std::unique_ptr<Impl> impl_;
    };

    /** A single graph operation owns all connected node operations. The executor only lends raw
     * reusable storage; it never starts, destroys or retains an independently owned operation.
     */
    class TaskGraphSender final
    {
    public:
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(),
            stdexec::set_error_t(TaskExecutorFailure),
            stdexec::set_stopped_t()>;

        template <class Receiver> class Operation final
        {
        public:
            using operation_state_concept = stdexec::operation_state_t;
            using CallerScheduler = decltype(stdexec::get_scheduler(stdexec::get_env(std::declval<Receiver&>())));

        private:
            struct NodeReceiver final
            {
                using receiver_concept = stdexec::receiver_t;
                Operation* operation;
                std::uint32_t index;

                [[nodiscard]] stdexec::env<> get_env() const noexcept
                {
                    return {};
                }

                void set_value() && noexcept
                {
                    operation->executeNode(index);
                }

                // Accepted graph nodes use never_stop_token; their schedulers stay available.
                void set_stopped() && noexcept
                {
                    std::terminate();
                }
            };

            using CpuOperation =
                stdexec::connect_result_t<stdexec::schedule_result_t<execution::CpuScheduler>, NodeReceiver>;
            using CallerOperation =
                stdexec::connect_result_t<stdexec::schedule_result_t<CallerScheduler>, NodeReceiver>;

            struct Node final
            {
                union Scheduled
                {
                    CpuOperation cpu;
                    CallerOperation caller;

                    Scheduled() noexcept {}

                    ~Scheduled() noexcept {}
                } scheduled;

                std::atomic<std::uint32_t> prerequisites;
            };

        public:
            Operation(TaskExecutor& executor, const TaskGraph& graph, Receiver receiver)
                : executor_(executor), graph_(graph), receiver_(std::move(receiver))
            {
            }

            Operation(const Operation&) = delete;
            Operation(Operation&&) = delete;

            void start() & noexcept
            {
                if (stdexec::get_stop_token(stdexec::get_env(receiver_)).stop_requested())
                {
                    stdexec::set_stopped(std::move(receiver_));
                    return;
                }
                if (!executor_.acquire())
                {
                    stdexec::set_error(std::move(receiver_), TaskExecutorFailure{});
                    return;
                }
                nodes_ = static_cast<Node*>(executor_.workspace(graph_.taskCount(), sizeof(Node), alignof(Node)));
                // Extra reference covers root publication and synchronous scheduler reentry.
                remaining_.store(graph_.taskCount() + 1, std::memory_order_relaxed);
                const auto cpu = executor_.cpu();
                const auto caller = stdexec::get_scheduler(stdexec::get_env(receiver_));
                for (std::uint32_t index = 0; index < graph_.taskCount(); ++index)
                {
                    auto& node = *std::construct_at(nodes_ + index);
                    node.prerequisites.store(graph_.initial_dependencies_[index], std::memory_order_relaxed);
                    if (graph_.tasks_[index].affinity == ETaskAffinity::CALLER_THREAD)
                    {
                        ::new (static_cast<void*>(&node.scheduled.caller))
                            CallerOperation(stdexec::connect(stdexec::schedule(caller), NodeReceiver{this, index}));
                    }
                    else
                    {
                        ::new (static_cast<void*>(&node.scheduled.cpu))
                            CpuOperation(stdexec::connect(stdexec::schedule(cpu), NodeReceiver{this, index}));
                    }
                }
                for (const auto root : graph_.roots_)
                {
                    startNode(root);
                }
                releaseCompletionReference();
            }

        private:
            void startNode(std::uint32_t index) noexcept
            {
                if (graph_.tasks_[index].affinity == ETaskAffinity::CALLER_THREAD)
                {
                    stdexec::start(nodes_[index].scheduled.caller);
                }
                else
                {
                    stdexec::start(nodes_[index].scheduled.cpu);
                }
            }

            void executeNode(std::uint32_t index) noexcept
            {
                graph_.tasks_[index].callable();
                const auto end = graph_.successor_offsets_[index + 1];
                for (auto edge = graph_.successor_offsets_[index]; edge < end; ++edge)
                {
                    const auto successor = graph_.successors_[edge];
                    // Last prerequisite acquires predecessor writes before publishing the node.
                    if (nodes_[successor].prerequisites.fetch_sub(1, std::memory_order_acq_rel) == 1)
                    {
                        startNode(successor);
                    }
                }
                // No graph/state access follows this release, including nested completions.
                releaseCompletionReference();
            }

            void releaseCompletionReference() noexcept
            {
                if (remaining_.fetch_sub(1, std::memory_order_acq_rel) != 1)
                {
                    return;
                }
                for (std::uint32_t index = 0; index < graph_.taskCount(); ++index)
                {
                    if (graph_.tasks_[index].affinity == ETaskAffinity::CALLER_THREAD)
                    {
                        std::destroy_at(&nodes_[index].scheduled.caller);
                    }
                    else
                    {
                        std::destroy_at(&nodes_[index].scheduled.cpu);
                    }
                    std::destroy_at(nodes_ + index);
                }
                // Receiver may synchronously start another graph or destroy this operation.
                executor_.release();
                stdexec::set_value(std::move(receiver_));
            }

            TaskExecutor& executor_;
            const TaskGraph& graph_;
            Receiver receiver_;
            Node* nodes_{};
            std::atomic<std::size_t> remaining_{};
        };

        template <class Receiver> [[nodiscard]] Operation<std::decay_t<Receiver>> connect(Receiver&& receiver) const
        {
            return {*executor_, *graph_, std::forward<Receiver>(receiver)};
        }

    private:
        friend class TaskExecutor;

        TaskGraphSender(TaskExecutor& executor, const TaskGraph& graph) noexcept : executor_(&executor), graph_(&graph)
        {
        }

        TaskExecutor* executor_;
        const TaskGraph* graph_;
    };
} // namespace lux::task

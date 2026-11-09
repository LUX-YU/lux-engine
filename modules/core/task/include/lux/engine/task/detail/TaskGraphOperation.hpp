#pragma once

#include <lux/engine/task/TaskExecutor.hpp>

#include <atomic>
#include <exception>
#include <memory>
#include <new>

namespace lux::task
{
    template <class Receiver> class TaskGraphSender::Operation final
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
                stdexec::set_error(std::move(receiver_), ETaskExecutorError::ALREADY_EXECUTING);
                return;
            }
            nodes_ = static_cast<Node*>(executor_.workspace(graph_.taskCount(), sizeof(Node), alignof(Node)));
            // Extra reference covers root publication and synchronous scheduler reentry.
            remaining_.store(graph_.taskCount() + 1, std::memory_order_relaxed);
            const auto cpu = executor_.cpuScheduler();
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

        TaskExecutor&           executor_;
        const TaskGraph&         graph_;
        Receiver                receiver_;
        Node*                   nodes_{};
        std::atomic<std::size_t> remaining_{};
    };

    template <class Receiver>
    TaskGraphSender::Operation<std::decay_t<Receiver>> TaskGraphSender::connect(Receiver&& receiver) const
    {
        return {*executor_, *graph_, std::forward<Receiver>(receiver)};
    }
} // namespace lux::task

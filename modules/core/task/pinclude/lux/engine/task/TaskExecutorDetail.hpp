#pragma once

#include <lux/engine/task/TaskExecutor.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace lux::task::detail
{
    inline constexpr std::uint32_t InvalidWorkerIndex = 0xffff'ffffU;

    struct ReadyStack final
    {
        std::atomic<std::uint32_t> head{InvalidTaskIndex};

        void reset() noexcept
        {
            head.store(InvalidTaskIndex, std::memory_order_relaxed);
        }

        void push(
            std::uint32_t task,
            std::uint32_t* next_ready
        ) noexcept
        {
            std::uint32_t observed = head.load(std::memory_order_relaxed);
            do
            {
                next_ready[task] = observed;
            }
            while (!head.compare_exchange_weak(
                observed,
                task,
                std::memory_order_release,
                std::memory_order_relaxed
            ));
        }

        [[nodiscard]] std::uint32_t pop(
            const std::uint32_t* next_ready
        ) noexcept
        {
            std::uint32_t observed = head.load(std::memory_order_acquire);
            while (observed != InvalidTaskIndex)
            {
                const std::uint32_t next = next_ready[observed];
                if (head.compare_exchange_weak(
                    observed,
                    next,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire
                ))
                {
                    return observed;
                }
            }
            return InvalidTaskIndex;
        }
    };

    struct TaskExecutorImpl
    {
        explicit TaskExecutorImpl(TaskExecutorConfig config);
        ~TaskExecutorImpl() noexcept;

        TaskExecutorImpl(const TaskExecutorImpl&) = delete;
        TaskExecutorImpl& operator=(const TaskExecutorImpl&) = delete;

        void reserveOrThrow(std::size_t task_capacity);

        [[nodiscard]] lux::cxx::expected<void, TaskExecutorFailure>
        reserve(std::size_t task_capacity) noexcept;

        [[nodiscard]] lux::cxx::expected<void, TaskExecutorFailure>
        execute(const TaskGraph& graph) noexcept;

        void workerLoop(std::uint32_t worker) noexcept;
        void executeTask(
            const TaskGraph& graph,
            std::uint32_t task,
            std::uint32_t worker
        ) noexcept;
        void schedule(
            const TaskGraph& graph,
            std::uint32_t task,
            std::uint32_t worker_hint
        ) noexcept;

        [[nodiscard]] std::uint32_t popWorkerTask(
            std::uint32_t preferred_worker
        ) noexcept;

        void signalCaller() noexcept
        {
            event.fetch_add(1U, std::memory_order_release);
            event.notify_one();
        }

        std::uint32_t worker_count{};
        std::size_t task_capacity{};

        std::unique_ptr<ReadyStack[]> worker_ready;
        ReadyStack caller_ready;
        std::vector<std::thread> workers;
        std::atomic<std::uint64_t> worker_event{};
        std::atomic<std::uint32_t> sleeping_workers{};

        std::unique_ptr<std::atomic<std::uint32_t>[]> remaining;
        std::unique_ptr<std::uint32_t[]> next_ready;

        std::atomic<const TaskGraph*> active_graph{nullptr};
        std::atomic<std::uint32_t> remaining_terminals{};
        std::atomic<std::uint32_t> round_robin{};
        std::atomic<std::uint64_t> event{};
        std::atomic_bool stopping{};
        std::atomic_bool executing{};
    };
}

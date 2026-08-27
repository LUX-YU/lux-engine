#include <lux/engine/task/TaskExecutorDetail.hpp>

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace lux::task::detail
{
    namespace
    {
        [[noreturn]] void contractFailure() noexcept
        {
            std::abort();
        }

        class ExecuteGuard final
        {
        public:
            explicit ExecuteGuard(std::atomic_bool& executing) noexcept : executing_(executing)
            {
            }

            ~ExecuteGuard() noexcept
            {
                executing_.store(false, std::memory_order_release);
            }

        private:
            std::atomic_bool& executing_;
        };
    }

    TaskExecutorImpl::TaskExecutorImpl(TaskExecutorConfig config) : worker_count(config.worker_count)
    {
        if (worker_count != 0U)
            worker_ready = std::make_unique<ReadyStack[]>(worker_count);

        if (config.initial_task_capacity != 0U)
            reserveOrThrow(config.initial_task_capacity);

        try
        {
            workers.reserve(worker_count);
            for (std::uint32_t worker{}; worker < worker_count; ++worker)
            {
                workers.emplace_back([this, worker]() noexcept { workerLoop(worker); });
            }
        }
        catch (...)
        {
            stopping.store(true, std::memory_order_release);
            worker_event.fetch_add(1U, std::memory_order_release);
            worker_event.notify_all();
            for (auto& worker : workers)
                if (worker.joinable())
                    worker.join();
            throw;
        }
    }

    TaskExecutorImpl::~TaskExecutorImpl() noexcept
    {
        stopping.store(true, std::memory_order_release);
        worker_event.fetch_add(1U, std::memory_order_release);
        worker_event.notify_all();
        for (auto& worker : workers)
            if (worker.joinable())
                worker.join();
    }

    void TaskExecutorImpl::reserveOrThrow(std::size_t capacity)
    {
        if (capacity <= task_capacity)
            return;
        auto new_remaining = std::make_unique<std::atomic<std::uint32_t>[]>(capacity);
        auto new_next = std::make_unique<std::uint32_t[]>(capacity);
        remaining = std::move(new_remaining);
        next_ready = std::move(new_next);
        task_capacity = capacity;
    }

    lux::cxx::expected<void, TaskExecutorFailure> TaskExecutorImpl::reserve(std::size_t capacity) noexcept
    {
        if (executing.load(std::memory_order_acquire))
        {
            return lux::cxx::unexpected(TaskExecutorFailure{ETaskExecutorError::ALREADY_EXECUTING});
        }
        try
        {
            reserveOrThrow(capacity);
            return {};
        }
        catch (...)
        {
            return lux::cxx::unexpected(TaskExecutorFailure{ETaskExecutorError::ALLOCATION_FAILURE});
        }
    }

    lux::cxx::expected<void, TaskExecutorFailure> TaskExecutorImpl::execute(const TaskGraph& graph) noexcept
    {
        bool expected = false;
        if (!executing.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            return lux::cxx::unexpected(TaskExecutorFailure{ETaskExecutorError::ALREADY_EXECUTING});
        }
        ExecuteGuard execute_guard(executing);

        try
        {
            reserveOrThrow(graph.taskCount());
        }
        catch (...)
        {
            return lux::cxx::unexpected(TaskExecutorFailure{ETaskExecutorError::ALLOCATION_FAILURE});
        }

        if (graph.taskCount() == 0U)
            return {};

        for (std::uint32_t task{}; task < graph.taskCount(); ++task)
        {
            remaining[task].store(graph.initial_dependencies_[task], std::memory_order_relaxed);
            next_ready[task] = InvalidTaskIndex;
        }
        for (std::uint32_t worker{}; worker < worker_count; ++worker)
            worker_ready[worker].reset();
        caller_ready.reset();

        round_robin.store(0U, std::memory_order_relaxed);
        remaining_terminals.store(graph.terminal_task_count_, std::memory_order_relaxed);
        active_graph.store(std::addressof(graph), std::memory_order_release);

        // Precomputed roots avoid scanning dependency counters every execution.
        for (const std::uint32_t root : graph.roots_)
            schedule(graph, root, InvalidWorkerIndex);

        // The caller owns caller-affine tasks. Background WORKER tasks are not
        // stolen by the caller; product code chooses worker_count accordingly.
        while (remaining_terminals.load(std::memory_order_acquire) != 0U)
        {
            const std::uint32_t caller_task = caller_ready.pop(next_ready.get());
            if (caller_task != InvalidTaskIndex)
            {
                executeTask(graph, caller_task, InvalidWorkerIndex);
                continue;
            }

            const std::uint64_t observed = event.load(std::memory_order_acquire);
            if (remaining_terminals.load(std::memory_order_acquire) == 0U)
                break;
            if (caller_ready.head.load(std::memory_order_acquire) != InvalidTaskIndex)
                continue;
            event.wait(observed, std::memory_order_acquire);
        }

        active_graph.store(nullptr, std::memory_order_release);
        return {};
    }

    void TaskExecutorImpl::workerLoop(std::uint32_t worker) noexcept
    {
        while (true)
        {
            if (stopping.load(std::memory_order_acquire))
                return;

            if (const TaskGraph* graph = active_graph.load(std::memory_order_acquire))
            {
                const std::uint32_t task = popWorkerTask(worker);
                if (task != InvalidTaskIndex)
                {
                    executeTask(*graph, task, worker);
                    continue;
                }
            }

            // Register as a sleeper before the final queue check. A publisher
            // either observes the sleeper and signals, or it published earlier
            // and this recheck observes the work without requiring a wakeup.
            sleeping_workers.fetch_add(1U, std::memory_order_acq_rel);
            const std::uint64_t observed = worker_event.load(std::memory_order_acquire);
            if (stopping.load(std::memory_order_acquire))
            {
                sleeping_workers.fetch_sub(1U, std::memory_order_acq_rel);
                return;
            }
            if (const TaskGraph* graph = active_graph.load(std::memory_order_acquire))
            {
                const std::uint32_t task = popWorkerTask(worker);
                if (task != InvalidTaskIndex)
                {
                    sleeping_workers.fetch_sub(1U, std::memory_order_acq_rel);
                    executeTask(*graph, task, worker);
                    continue;
                }
            }
            worker_event.wait(observed, std::memory_order_acquire);
            sleeping_workers.fetch_sub(1U, std::memory_order_acq_rel);
        }
    }

    std::uint32_t TaskExecutorImpl::popWorkerTask(std::uint32_t preferred_worker) noexcept
    {
        if (worker_count == 0U)
            return InvalidTaskIndex;

        if (preferred_worker < worker_count)
        {
            const auto local = worker_ready[preferred_worker].pop(next_ready.get());
            if (local != InvalidTaskIndex)
                return local;
        }

        const std::uint32_t start = preferred_worker < worker_count
                                        ? preferred_worker + 1U
                                        : round_robin.fetch_add(1U, std::memory_order_relaxed);

        // Stealing is paid only after the local queue misses.
        for (std::uint32_t offset{}; offset < worker_count; ++offset)
        {
            const std::uint32_t victim = (start + offset) % worker_count;
            if (victim == preferred_worker)
                continue;
            const auto stolen = worker_ready[victim].pop(next_ready.get());
            if (stolen != InvalidTaskIndex)
                return stolen;
        }
        return InvalidTaskIndex;
    }

    void TaskExecutorImpl::schedule(const TaskGraph& graph, std::uint32_t task, std::uint32_t worker_hint) noexcept
    {
        if (task >= graph.tasks_.size())
            contractFailure();

        const auto affinity = graph.tasks_[task].affinity;
        if (affinity == ETaskAffinity::CALLER_THREAD || worker_count == 0U)
        {
            caller_ready.push(task, next_ready.get());
            signalCaller();
            return;
        }

        const std::uint32_t target = worker_hint < worker_count
                                         ? worker_hint
                                         : round_robin.fetch_add(1U, std::memory_order_relaxed) % worker_count;
        worker_ready[target].push(task, next_ready.get());
        if (sleeping_workers.load(std::memory_order_acquire) != 0U)
        {
            worker_event.fetch_add(1U, std::memory_order_release);
            worker_event.notify_one();
        }
    }

    void TaskExecutorImpl::executeTask(const TaskGraph& graph, std::uint32_t task, std::uint32_t worker) noexcept
    {
        if (task >= graph.tasks_.size())
            contractFailure();

        if (worker != InvalidWorkerIndex && graph.tasks_[task].affinity == ETaskAffinity::CALLER_THREAD)
        {
            contractFailure();
        }

        graph.tasks_[task].callable();

        const std::uint32_t begin = graph.successor_offsets_[task];
        const std::uint32_t end = graph.successor_offsets_[task + 1U];
        for (std::uint32_t edge = begin; edge < end; ++edge)
        {
            const std::uint32_t successor = graph.successors_[edge];
            // The final predecessor acquires the release sequence formed by all
            // predecessor RMWs, then publishes the successor into a ready stack.
            if (remaining[successor].fetch_sub(1U, std::memory_order_acq_rel) == 1U)
            {
                schedule(graph, successor, worker);
            }
        }

        if (begin == end && remaining_terminals.fetch_sub(1U, std::memory_order_acq_rel) == 1U)
        {
            event.fetch_add(1U, std::memory_order_release);
            event.notify_all();
        }
    }
}

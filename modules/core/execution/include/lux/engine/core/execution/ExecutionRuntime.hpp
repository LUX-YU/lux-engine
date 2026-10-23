#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/core/execution/RunLoop.hpp>
#include <lux/engine/core/execution/Scheduler.hpp>
#include <lux/engine/core/execution/Task.hpp>
#include <lux/engine/core/execution/Timer.hpp>
#include <lux/engine/core/execution/visibility.h>

#include <stdexec/execution.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace lux::execution
{
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

    class LUX_CORE_EXECUTION_PUBLIC ExecutionRuntime final
    {
    public:
        using CreateResult = lux::cxx::expected<ExecutionRuntime, EExecutionError>;

        [[nodiscard]] static CreateResult create(ExecutionRuntimeConfig config) noexcept;

        ~ExecutionRuntime() noexcept;
        ExecutionRuntime(ExecutionRuntime&& other) noexcept;
        ExecutionRuntime& operator=(ExecutionRuntime&& other) noexcept;
        ExecutionRuntime(const ExecutionRuntime&) = delete;
        ExecutionRuntime& operator=(const ExecutionRuntime&) = delete;

        // Borrowed CPU pool; stop/join all external users before join(). Not callable after join.
        [[nodiscard]] CpuScheduler cpu() const noexcept;
        // Owner-thread admission fact, before shutdown/join mutates worker storage.
        [[nodiscard]] std::size_t cpuConcurrency() const noexcept;
        [[nodiscard]] TimerClient timer() const noexcept;
        [[nodiscard]] lux::cxx::expected<BlockingScheduler, EExecutionError> blocking() const noexcept;

        // The returned Task is a nonblocking cancellation handle, not an operation owner.
        // Work and captured inputs remain in this Runtime until their real terminal completion.
        template <class Factory>
        [[nodiscard]] lux::cxx::expected<Task, EExecutionError> submit(
            TaskOptions options,
            Factory&& make_sender
        ) noexcept
        {
            static_assert(std::is_nothrow_invocable_v<Factory, TaskReporter>);
            auto admitted = detail::admitTask(*tasks_, std::move(options));
            if (!admitted)
            {
                return lux::cxx::unexpected(admitted.error());
            }
            auto& record = **admitted;
            Task task{*tasks_, detail::taskId(record)};
            TaskReporter reporter{&record};
            detail::spawnTask(record, reporter, std::invoke(std::forward<Factory>(make_sender), reporter));
            return task;
        }

        // Borrowed scheduler for this Runtime's application thread. It delivers determined
        // results even after task cancellation; business adoption checks its own target/revision.
        [[nodiscard]] RunLoop::Scheduler mainScheduler() noexcept;
        // Runs a batch bounded by the entry count, outside queue locks. Returns consumed schedule
        // nodes, including revoked coalesced notifications; concurrent publication can join the batch.
        [[nodiscard]] lux::cxx::expected<std::size_t, EExecutionError> dispatchMainCompletions() noexcept;
        // Tasks and Main work are quiescent; external producer tails still require their join.
        [[nodiscard]] bool settled() const noexcept;
        [[nodiscard]] lux::cxx::expected<std::size_t, EExecutionError> dispatchTaskEvents() noexcept;
        [[nodiscard]] std::optional<TaskInfo> taskInfo(TaskId id) const noexcept;
        [[nodiscard]] std::vector<TaskInfo> taskInfos() const noexcept;
        [[nodiscard]] bool requestStop(TaskId id) noexcept;
        using TaskObserver = void (*)(void*, std::span<const TaskId>, bool resync) noexcept;
        void setTaskObserver(void* context, TaskObserver observer) noexcept;

        // Observe before checking work, then pass the epoch to waitForWork to avoid a lost wake.
        [[nodiscard]] std::uint64_t wakeEpoch() const noexcept;
        // Ready Main continuations, changed monitoring or a publication returning from its wake.
        // A publisher tail must finish before sleeping for another event. Unrelated worker tasks
        // awaiting work do not make this true.
        [[nodiscard]] bool hasReadyWork() const noexcept;
        void wake() noexcept;
        void setWake(void (*wake)() noexcept) noexcept;
        void waitForWork(
            std::uint64_t observed,
            std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()
        ) noexcept;
        [[nodiscard]] lux::cxx::expected<void, EExecutionError> waitUntil(
            void* context,
            bool (*ready)(void*) noexcept
        ) noexcept;

        template <class Predicate>
        [[nodiscard]] lux::cxx::expected<void, EExecutionError> waitUntil(Predicate&& ready) noexcept
        {
            return waitUntil(
                std::addressof(ready),
                [](void* value) noexcept { return (*static_cast<std::remove_reference_t<Predicate>*>(value))(); }
            );
        }

        void requestStop() noexcept;
        [[nodiscard]] lux::cxx::expected<void, EExecutionError> join() noexcept;

    private:
        friend class CompletionWork;
        friend class TaskScope;
        friend struct detail::TaskRuntime;
        [[nodiscard]] lux::cxx::expected<void, EExecutionError> validateWait() const noexcept;
        ExecutionRuntime(
            std::shared_ptr<detail::ExecutionState> state,
            TimerQueue timer,
            std::size_t task_capacity,
            std::size_t history_capacity
        ) noexcept;
        void finishShutdown() noexcept;

        std::shared_ptr<detail::ExecutionState> state_;
        TimerQueue timer_;
        std::unique_ptr<detail::TaskRuntime> tasks_;
    };

    static_assert(stdexec::scheduler<CpuScheduler>);
    static_assert(stdexec::scheduler<BlockingScheduler>);
} // namespace lux::execution

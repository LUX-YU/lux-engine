#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/core/execution/ExecutionConfig.hpp>
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
    namespace detail
    {
        [[nodiscard]] LUX_CORE_EXECUTION_PUBLIC bool admitExecution(ExecutionState&) noexcept;
        LUX_CORE_EXECUTION_PUBLIC void releaseExecution(ExecutionState&) noexcept;
        [[nodiscard]] LUX_CORE_EXECUTION_PUBLIC stdexec::simple_counting_scope&
        executionScope(ExecutionState&) noexcept;

        // Only the outer spawn allocation counts an accepted operation. Nested sender allocations
        // use their ordinary allocator. stdexec owns construction, destruction and scope association.
        template <class T> struct ExecutionAllocator final
        {
            using value_type = T;
            ExecutionState* state;

            explicit ExecutionAllocator(ExecutionState& value) noexcept : state(&value) {}

            template <class U> ExecutionAllocator(const ExecutionAllocator<U>& other) noexcept : state(other.state) {}

            [[nodiscard]] T* allocate(std::size_t count)
            {
                return std::allocator<T>{}.allocate(count);
            }

            void deallocate(T* memory, std::size_t count) noexcept
            {
                std::allocator<T>{}.deallocate(memory, count);
                releaseExecution(*state);
            }

            template <class U> [[nodiscard]] bool operator==(const ExecutionAllocator<U>& other) const noexcept
            {
                return state == other.state;
            }
        };
    } // namespace detail

    class LUX_CORE_EXECUTION_PUBLIC ExecutionRuntime final
    {
    public:
        using CreateResult = lux::cxx::expected<std::unique_ptr<ExecutionRuntime>, EExecutionError>;

        [[nodiscard]] static CreateResult create(ExecutionRuntimeConfig config) noexcept;

        // One explicitly created instance per process. The owner outlives every scheduler borrower.
        // Access before successful create or after destruction is a programming error.
        [[nodiscard]] static ExecutionRuntime& instance() noexcept;
        [[nodiscard]] bool isMainThread() const noexcept;
        [[nodiscard]] bool isStopping() const noexcept;

        ~ExecutionRuntime() noexcept;
        ExecutionRuntime(ExecutionRuntime&&) = delete;
        ExecutionRuntime& operator=(ExecutionRuntime&&) = delete;
        ExecutionRuntime(const ExecutionRuntime&) = delete;
        ExecutionRuntime& operator=(const ExecutionRuntime&) = delete;

        // Borrowed CPU pool; settle and join all external users before destroying the Runtime.
        [[nodiscard]] CpuScheduler cpuScheduler() const noexcept;
        // Borrows a specific existing worker. The index must be below cpuConcurrency().
        [[nodiscard]] CpuScheduler cpuSchedulerOnWorker(std::size_t index) const noexcept;
        // Owner-thread admission fact, before shutdown/join mutates worker storage.
        [[nodiscard]] std::size_t cpuConcurrency() const noexcept;
        [[nodiscard]] TimerClient timer() const noexcept;
        [[nodiscard]] lux::cxx::expected<BlockingScheduler, EExecutionError> blockingIoScheduler() const noexcept;

        // Starts finite work without TaskId, monitoring or a per-operation stop source. Consume
        // business results and normalize errors/stopped inside sender before spawning it. Accepted
        // work and its captures remain covered by settled()/join(); requestStop closes new admission.
        // The caller keeps this Runtime alive through the submission call, including synchronous completion.
        template <class Sender> [[nodiscard]] lux::cxx::expected<void, EExecutionError> spawn(Sender sender) noexcept
        {
            return spawnWithAllocator(std::move(sender), detail::ExecutionAllocator<std::byte>{*state_});
        }

        // The returned Task is a nonblocking cancellation handle, not an operation owner.
        // Work and captured inputs remain in this Runtime until their real terminal completion.
        template <class Factory>
        [[nodiscard]] lux::cxx::expected<Task, EExecutionError>
        submit(TaskOptions options, Factory&& make_sender) noexcept
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
        // The application loop calls dispatch at a safe point, never while a scene graph is executing.
        // Delivers main-thread continuations and task observations; nested dispatch is rejected.
        [[nodiscard]] lux::cxx::expected<void, EExecutionError> dispatch() noexcept;
        // Tasks and Main work are quiescent; external producer tails still require their join.
        [[nodiscard]] bool settled() const noexcept;
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
        // Notification only: callback must not block, rebind or dispatch this Runtime. Main-thread
        // replacement waits for old callbacks to return; clear before destroying the platform target.
        void setWake(void (*wake)() noexcept) noexcept;
        void waitForWork(
            std::uint64_t observed,
            std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()
        ) noexcept;
        [[nodiscard]] lux::cxx::expected<void, EExecutionError>
        waitUntil(void* context, bool (*ready)(void*) noexcept) noexcept;

        template <class Predicate>
        [[nodiscard]] lux::cxx::expected<void, EExecutionError> waitUntil(Predicate&& ready) noexcept
        {
            return waitUntil(
                std::addressof(ready),
                [](void* value) noexcept { return (*static_cast<std::remove_reference_t<Predicate>*>(value))(); }
            );
        }

    private:
        void requestStop() noexcept;
        [[nodiscard]] lux::cxx::expected<void, EExecutionError> join() noexcept;

        template <class Sender, class Allocator>
        [[nodiscard]] cxx::expected<void, EExecutionError>
        spawnWithAllocator(Sender sender, Allocator allocator) noexcept
        {
            if (!detail::admitExecution(*state_))
            {
                return lux::cxx::unexpected(EExecutionError::STOPPING);
            }
            auto& scope = detail::executionScope(*state_);
            // The extra association covers the submission tail if start completes synchronously.
            auto submitting = scope.get_token().try_associate();
            stdexec::spawn(
                stdexec::write_env(
                    std::move(sender),
                    stdexec::prop{stdexec::get_allocator, std::allocator<std::byte>{}}
                ),
                scope.get_token(),
                stdexec::env{stdexec::prop{stdexec::get_allocator, std::move(allocator)}}
            );
            detail::releaseExecution(*state_);
            return {};
        }

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

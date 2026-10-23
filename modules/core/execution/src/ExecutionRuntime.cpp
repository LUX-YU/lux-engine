#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/core/execution/detail/ExecutionState.hpp>
#include <lux/engine/core/execution/detail/TaskState.hpp>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <system_error>
#include <vector>

namespace lux::execution
{
    namespace detail
    {
        bool RequestQueue::push(ScheduleRequest* request) noexcept
        {
            if (count == values.size())
            {
                return false;
            }
            values[tail] = request;
            tail = (tail + 1U) % values.size();
            ++count;
            return true;
        }

        ScheduleRequest* RequestQueue::pop() noexcept
        {
            if (count == 0U)
            {
                return nullptr;
            }
            auto* result = values[head];
            values[head] = nullptr;
            head = (head + 1U) % values.size();
            --count;
            return result;
        }

        ExecutionState::ExecutionState(ExecutionRuntimeConfig config, std::thread::id thread)
            : main_loop([](void* state) noexcept { static_cast<ExecutionState*>(state)->wake(); }, this),
              blocking_queue(config.blocking ? config.blocking->queue_capacity : 0U),
              blocking_enabled(config.blocking.has_value()), main_thread(thread)
        {
        }

        void ExecutionState::wake() noexcept
        {
            wake_epoch.fetch_add(1, std::memory_order_release);
            if (!wake_pending.exchange(true, std::memory_order_acq_rel))
            {
                wake_signal.release();
            }
            if (const auto callback = wake_callback.load(std::memory_order_acquire))
            {
                callback();
            }
        }

        namespace
        {
            void blockingWorker(ExecutionState& state) noexcept
            {
                for (;;)
                {
                    ScheduleRequest* request{};
                    bool stopping{};
                    {
                        std::unique_lock lock{state.blocking_mutex};
                        state.blocking_ready.wait(
                            lock,
                            [&state]
                            {
                                return state.blocking_queue.count != 0U ||
                                       state.phase.load(std::memory_order_acquire) != EExecutionState::ACTIVE;
                            }
                        );
                        stopping = state.phase.load(std::memory_order_acquire) != EExecutionState::ACTIVE;
                        request = state.blocking_queue.pop();
                        if (request == nullptr && stopping)
                        {
                            return;
                        }
                    }

                    const bool stopped = stopping || request->cancel_requested.load(std::memory_order_acquire);
                    request->complete(request, stopped);
                }
            }

            void stopState(const std::shared_ptr<ExecutionState>& state) noexcept
            {
                if (!state)
                {
                    return;
                }
                auto expected = EExecutionState::ACTIVE;
                if (state->phase.compare_exchange_strong(
                        expected,
                        EExecutionState::STOPPING,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire
                    ))
                {
                    state->blocking_ready.notify_all();
                }
            }

            void joinWorkers(ExecutionState& state) noexcept
            {
                // All externally borrowed CPU operations are already quiescent. The pool's
                // destructor joins its threads, including their completion callback tails.
                state.cpu_pool.reset();
                for (auto& worker : state.blocking_workers)
                {
                    if (worker.joinable())
                    {
                        worker.join();
                    }
                }
                state.blocking_workers.clear();
            }
        } // namespace

        ScheduleSubmitResult submitBlocking(
            const std::shared_ptr<ExecutionState>& state,
            ScheduleRequest& request
        ) noexcept
        {
            if (!state || state->phase.load(std::memory_order_acquire) != EExecutionState::ACTIVE)
            {
                return lux::cxx::unexpected(EExecutionError::STOPPING);
            }

            if (!state->blocking_enabled)
            {
                return lux::cxx::unexpected(EExecutionError::CAPABILITY_UNAVAILABLE);
            }
            std::lock_guard lock{state->blocking_mutex};
            if (state->phase.load(std::memory_order_acquire) != EExecutionState::ACTIVE)
            {
                return lux::cxx::unexpected(EExecutionError::STOPPING);
            }
            if (!state->blocking_queue.push(&request))
            {
                return lux::cxx::unexpected(EExecutionError::CAPACITY_EXCEEDED);
            }
            state->blocking_ready.notify_one();
            return {};
        }

    } // namespace detail

    ExecutionRuntime::ExecutionRuntime(
        std::shared_ptr<detail::ExecutionState> state,
        TimerQueue timer,
        std::size_t task_capacity,
        std::size_t history_capacity
    ) noexcept
        : state_(std::move(state)), timer_(std::move(timer)),
          tasks_(std::make_unique<detail::TaskRuntime>(*this, task_capacity, history_capacity))
    {
    }

    ExecutionRuntime::CreateResult ExecutionRuntime::create(ExecutionRuntimeConfig config) noexcept
    {
        const bool is_invalid_blocking =
            config.blocking && (config.blocking->concurrency == 0U || config.blocking->queue_capacity == 0U);
        const bool is_invalid_config = config.cpu_concurrency == 0U || config.cpu_concurrency > UINT32_MAX ||
                                       config.timer.capacity == 0U || config.task_capacity == 0 || is_invalid_blocking;
        if (is_invalid_config)
        {
            return lux::cxx::unexpected(EExecutionError::INVALID_ARGUMENT);
        }

        auto timer = TimerQueue::create(config.timer);
        if (!timer)
        {
            const auto code = timer.error() == ETimerError::ALLOCATION_FAILURE ? EExecutionError::ALLOCATION_FAILURE
                              : timer.error() == ETimerError::WORKER_CREATION_FAILURE
                                  ? EExecutionError::WORKER_CREATION_FAILURE
                                  : EExecutionError::BACKEND_FAILURE;
            return lux::cxx::unexpected(code);
        }

        auto state = std::make_shared<detail::ExecutionState>(config, std::this_thread::get_id());
        if (config.blocking)
        {
            state->blocking_workers.reserve(config.blocking->concurrency);
        }

        try
        {
            state->cpu_pool =
                std::make_unique<exec::static_thread_pool>(static_cast<std::uint32_t>(config.cpu_concurrency));
            if (config.blocking)
            {
                for (std::size_t index{}; index < config.blocking->concurrency; ++index)
                {
                    state->blocking_workers.emplace_back([raw = state.get()] { detail::blockingWorker(*raw); });
                }
            }
            return ExecutionRuntime{
                std::move(state),
                std::move(*timer),
                config.task_capacity,
                config.task_history_capacity
            };
        }
        catch (const std::system_error&)
        {
            detail::stopState(state);
            detail::joinWorkers(*state);
            return lux::cxx::unexpected(EExecutionError::WORKER_CREATION_FAILURE);
        }
    }

    ExecutionRuntime::~ExecutionRuntime() noexcept
    {
        if (!state_)
        {
            return;
        }
        if (state_->main_thread != std::this_thread::get_id())
        {
            std::terminate();
        }
        finishShutdown();
    }

    void ExecutionRuntime::finishShutdown() noexcept
    {
        if (state_->phase.load(std::memory_order_acquire) == detail::EExecutionState::JOINED)
        {
            return;
        }
        requestStop();
        while (true)
        {
            const auto epoch = wakeEpoch();
            if (!dispatchMainCompletions())
            {
                std::terminate();
            }
            auto joined = join();
            if (joined || joined.error() == EExecutionError::ALREADY_JOINED)
            {
                return;
            }
            if (joined.error() != EExecutionError::WORK_PENDING)
            {
                std::terminate();
            }
            waitForWork(epoch);
        }
    }

    ExecutionRuntime::ExecutionRuntime(ExecutionRuntime&& other) noexcept
        : state_(std::move(other.state_)), timer_(std::move(other.timer_)), tasks_(std::move(other.tasks_))
    {
        if (tasks_)
        {
            tasks_->moveTo(*this);
        }
    }

    ExecutionRuntime& ExecutionRuntime::operator=(ExecutionRuntime&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }
        if (state_)
        {
            if (state_->main_thread != std::this_thread::get_id())
            {
                std::terminate();
            }
            finishShutdown();
        }
        state_ = std::move(other.state_);
        timer_ = std::move(other.timer_);
        tasks_ = std::move(other.tasks_);
        if (tasks_)
        {
            tasks_->moveTo(*this);
        }
        return *this;
    }

    std::size_t ExecutionRuntime::cpuConcurrency() const noexcept
    {
        return state_ && state_->cpu_pool ? state_->cpu_pool->available_parallelism() : 0;
    }

    CpuScheduler ExecutionRuntime::cpu() const noexcept
    {
        return state_->cpu_pool->get_scheduler();
    }

    RunLoop::Scheduler ExecutionRuntime::mainScheduler() noexcept
    {
        return state_->main_loop.scheduler();
    }

    bool ExecutionRuntime::settled() const noexcept
    {
        return tasks_->settled() && state_->main_loop.empty() && state_->main_loop.activePublishers() == 0;
    }

    lux::cxx::expected<std::size_t, EExecutionError> ExecutionRuntime::dispatchMainCompletions() noexcept
    {
        if (!tasks_ || state_->main_thread != std::this_thread::get_id())
        {
            return lux::cxx::unexpected(EExecutionError::WRONG_THREAD);
        }
        if (state_->dispatching_main)
        {
            return lux::cxx::unexpected(EExecutionError::INVALID_STATE);
        }
        state_->dispatching_main = true;
        const auto count = state_->main_loop.executeReady(state_->main_loop.size());
        state_->dispatching_main = false;
        return count;
    }

    lux::cxx::expected<std::size_t, EExecutionError> ExecutionRuntime::dispatchTaskEvents() noexcept
    {
        if (!tasks_ || state_->main_thread != std::this_thread::get_id())
        {
            return lux::cxx::unexpected(EExecutionError::WRONG_THREAD);
        }
        if (tasks_->dispatching || state_->dispatching_main)
        {
            return lux::cxx::unexpected(EExecutionError::INVALID_STATE);
        }
        return tasks_->dispatch();
    }

    std::optional<TaskInfo> ExecutionRuntime::taskInfo(TaskId id) const noexcept
    {
        return tasks_->info(id);
    }

    std::vector<TaskInfo> ExecutionRuntime::taskInfos() const noexcept
    {
        return tasks_->infos();
    }

    bool ExecutionRuntime::requestStop(TaskId id) noexcept
    {
        return tasks_->requestStop(id);
    }

    void ExecutionRuntime::setTaskObserver(void* context, TaskObserver observer) noexcept
    {
        tasks_->requireMainThread();
        // Installation is an exclusive application responsibility, not last-writer-wins registration.
        const bool is_partial_observer = (context == nullptr) != (observer == nullptr);
        const bool is_other_observer = observer && tasks_->observer && tasks_->observer_context != context;
        if (is_partial_observer || is_other_observer)
        {
            std::terminate();
        }
        tasks_->observer_context = context;
        tasks_->observer = observer;
    }

    TimerClient ExecutionRuntime::timer() const noexcept
    {
        return timer_.client();
    }

    lux::cxx::expected<BlockingScheduler, EExecutionError> ExecutionRuntime::blocking() const noexcept
    {
        if (!state_ || !state_->blocking_enabled)
        {
            return lux::cxx::unexpected(EExecutionError::CAPABILITY_UNAVAILABLE);
        }
        return BlockingScheduler{state_};
    }

    std::uint64_t ExecutionRuntime::wakeEpoch() const noexcept
    {
        return state_->wake_epoch.load(std::memory_order_acquire);
    }

    bool ExecutionRuntime::hasReadyWork() const noexcept
    {
        return tasks_->hasWork() || !state_->main_loop.empty();
    }

    void ExecutionRuntime::wake() noexcept
    {
        state_->wake();
    }

    void ExecutionRuntime::setWake(void (*wake)() noexcept) noexcept
    {
        if (state_->main_thread != std::this_thread::get_id())
        {
            std::terminate();
        }
        state_->wake_callback.store(wake, std::memory_order_release);
    }

    void ExecutionRuntime::waitForWork(std::uint64_t observed, std::chrono::steady_clock::time_point deadline) noexcept
    {
        if (state_->main_thread != std::this_thread::get_id() || state_->dispatching_main)
        {
            std::terminate();
        }
        while (wakeEpoch() == observed && state_->main_loop.empty())
        {
            if (deadline == std::chrono::steady_clock::time_point::max())
            {
                state_->wake_signal.acquire();
            }
            else if (!state_->wake_signal.try_acquire_until(deadline))
            {
                return;
            }
            // Acquire a coalesced producer's epoch publication before checking for more work.
            state_->wake_pending.exchange(false, std::memory_order_acq_rel);
        }
    }

    lux::cxx::expected<void, EExecutionError> ExecutionRuntime::validateWait() const noexcept
    {
        if (!state_ || state_->main_thread != std::this_thread::get_id())
        {
            return lux::cxx::unexpected(EExecutionError::WRONG_THREAD);
        }
        if (state_->dispatching_main)
        {
            return lux::cxx::unexpected(EExecutionError::INVALID_STATE);
        }
        return {};
    }

    lux::cxx::expected<void, EExecutionError> ExecutionRuntime::waitUntil(
        void* context,
        bool (*ready)(void*) noexcept
    ) noexcept
    {
        if (!state_ || state_->main_thread != std::this_thread::get_id())
        {
            return lux::cxx::unexpected(EExecutionError::WRONG_THREAD);
        }
        if (ready(context))
        {
            return {};
        }
        if (state_->dispatching_main)
        {
            return lux::cxx::unexpected(EExecutionError::INVALID_STATE);
        }
        while (!ready(context))
        {
            const auto epoch = wakeEpoch();
            const auto dispatched = dispatchMainCompletions();
            if (!dispatched)
            {
                return lux::cxx::unexpected(dispatched.error());
            }
            if (!ready(context))
            {
                waitForWork(epoch);
            }
        }
        return {};
    }

    void ExecutionRuntime::requestStop() noexcept
    {
        if (!state_)
        {
            return;
        }
        state_->stop_requested = true;
        tasks_->stop();
        timer_.requestStop();
    }

    lux::cxx::expected<void, EExecutionError> ExecutionRuntime::join() noexcept
    {
        if (!state_)
        {
            return lux::cxx::unexpected(EExecutionError::ALREADY_JOINED);
        }
        if (state_->main_thread != std::this_thread::get_id())
        {
            return lux::cxx::unexpected(EExecutionError::WRONG_THREAD);
        }

        const auto phase = state_->phase.load(std::memory_order_acquire);
        if (phase == detail::EExecutionState::JOINED)
        {
            return lux::cxx::unexpected(EExecutionError::ALREADY_JOINED);
        }
        if (!state_->stop_requested)
        {
            return lux::cxx::unexpected(EExecutionError::INVALID_STATE);
        }
        if (!settled())
        {
            return lux::cxx::unexpected(EExecutionError::WORK_PENDING);
        }
        // No admitted submission or Main continuation remains. Only stdexec's association
        // release can still be returning from deallocation; joining cannot depend on this thread.
        tasks_->execution_scope.close();
        static_cast<void>(stdexec::sync_wait(tasks_->execution_scope.join()));

        timer_.requestStop();
        detail::stopState(state_);
        detail::joinWorkers(*state_);
        state_->phase.store(detail::EExecutionState::JOINED, std::memory_order_release);
        return {};
    }
} // namespace lux::execution

#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/process/CompletionWork.hpp>
#include <lux/engine/process/detail/TaskState.hpp>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <system_error>
#include <vector>

namespace lux::process
{
    namespace detail
    {
        enum class EExecutionState : std::uint8_t
        {
            ACTIVE,
            STOPPING,
            JOINED,
        };

        struct RequestQueue final
        {
            explicit RequestQueue(std::size_t capacity) : values(capacity) {}

            [[nodiscard]] bool push(ScheduleRequest* request) noexcept
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

            [[nodiscard]] ScheduleRequest* pop() noexcept
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

            std::vector<ScheduleRequest*> values;
            std::size_t head{};
            std::size_t tail{};
            std::size_t count{};
        };

        struct CompletionWorkState final
        {
            std::weak_ptr<ExecutionState> runtime;
            void* owner{};
            void (*run)(void*) noexcept {};
            bool queued{}, cancelled{}, executing{};
        };

        struct ExecutionState final
        {
            ExecutionState(ExecutionRuntimeConfig config, std::thread::id owner)
                : cpu_queue(config.cpu_queue_capacity),
                  blocking_queue(config.blocking ? config.blocking->queue_capacity : 0U),
                  blocking_enabled(config.blocking.has_value()), owner_thread(owner)
            {}

            std::atomic<EExecutionState> phase{EExecutionState::ACTIVE};
            std::mutex cpu_mutex;
            std::condition_variable cpu_ready;
            RequestQueue cpu_queue;
            std::vector<std::jthread> workers;
            std::mutex completion_mutex;
            std::condition_variable completion_ready;
            std::vector<std::shared_ptr<CompletionWorkState>> ready_completion, completion_batch;
            std::size_t completion_count{};
            bool collecting_completions{};
            std::uint64_t wake_epoch{};
            std::atomic<void (*)() noexcept> wake_callback{};
            bool stop_requested{};
            std::mutex blocking_mutex;
            std::condition_variable blocking_ready;
            RequestQueue blocking_queue;
            std::vector<std::jthread> blocking_workers;
            bool blocking_enabled{};
            std::thread::id owner_thread;
        };

        namespace
        {
            void cpuWorker(ExecutionState& state) noexcept
            {
                for (;;)
                {
                    ScheduleRequest* request{};
                    bool stopping{};
                    {
                        std::unique_lock lock{state.cpu_mutex};
                        state.cpu_ready.wait(lock, [&state] {
                            return state.cpu_queue.count != 0U ||
                                   state.phase.load(std::memory_order_acquire) != EExecutionState::ACTIVE;
                        });
                        stopping = state.phase.load(std::memory_order_acquire) != EExecutionState::ACTIVE;
                        request = state.cpu_queue.pop();
                        if (request == nullptr && stopping)
                        {
                            return;
                        }
                    }

                    const bool stopped = stopping || request->cancel_requested.load(std::memory_order_acquire);
                    request->complete(request, stopped);
                }
            }

            void blockingWorker(ExecutionState& state) noexcept
            {
                for (;;)
                {
                    ScheduleRequest* request{};
                    bool stopping{};
                    {
                        std::unique_lock lock{state.blocking_mutex};
                        state.blocking_ready.wait(lock, [&state] {
                            return state.blocking_queue.count != 0U ||
                                   state.phase.load(std::memory_order_acquire) != EExecutionState::ACTIVE;
                        });
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
                    state->cpu_ready.notify_all();
                    state->blocking_ready.notify_all();
                }
            }

            void joinWorkers(ExecutionState& state) noexcept
            {
                for (auto& worker : state.workers)
                {
                    if (worker.joinable())
                    {
                        worker.join();
                    }
                }
                state.workers.clear();
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

        ScheduleSubmitResult submitSchedule(
            const std::shared_ptr<ExecutionState>& state,
            EExecutionQueue queue,
            ScheduleRequest& request
        ) noexcept
        {
            if (!state || state->phase.load(std::memory_order_acquire) != EExecutionState::ACTIVE)
            {
                return lux::cxx::unexpected(EExecutionError::STOPPING);
            }

            if (queue == EExecutionQueue::CPU)
            {
                std::lock_guard lock{state->cpu_mutex};
                if (state->phase.load(std::memory_order_acquire) != EExecutionState::ACTIVE)
                {
                    return lux::cxx::unexpected(EExecutionError::STOPPING);
                }
                if (!state->cpu_queue.push(&request))
                {
                    return lux::cxx::unexpected(EExecutionError::CAPACITY_EXCEEDED);
                }
                state->cpu_ready.notify_one();
                return {};
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
    {}

    ExecutionRuntime::CreateResult ExecutionRuntime::create(ExecutionRuntimeConfig config) noexcept
    {
        const bool is_invalid_blocking =
            config.blocking && (config.blocking->concurrency == 0U || config.blocking->queue_capacity == 0U);
        const bool is_invalid_config = config.cpu_concurrency == 0U || config.cpu_queue_capacity == 0U ||
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
        state->workers.reserve(config.cpu_concurrency);
        if (config.blocking)
            state->blocking_workers.reserve(config.blocking->concurrency);

        try
        {
            for (std::size_t index{}; index < config.cpu_concurrency; ++index)
            {
                state->workers.emplace_back([raw = state.get()] { detail::cpuWorker(*raw); });
            }
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
        if (state_->owner_thread != std::this_thread::get_id())
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
            if (!collectCompletions())
                std::terminate();
            auto joined = join();
            if (joined || joined.error() == EExecutionError::ALREADY_JOINED)
                return;
            if (joined.error() != EExecutionError::WORK_PENDING)
                std::terminate();
            waitForWork(epoch);
        }
    }

    ExecutionRuntime::ExecutionRuntime(ExecutionRuntime&& other) noexcept
        : state_(std::move(other.state_)), timer_(std::move(other.timer_)), tasks_(std::move(other.tasks_))
    {
        if (tasks_)
            tasks_->moveTo(*this);
    }

    ExecutionRuntime& ExecutionRuntime::operator=(ExecutionRuntime&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }
        if (state_)
        {
            if (state_->owner_thread != std::this_thread::get_id())
            {
                std::terminate();
            }
            finishShutdown();
        }
        state_ = std::move(other.state_);
        timer_ = std::move(other.timer_);
        tasks_ = std::move(other.tasks_);
        if (tasks_)
            tasks_->moveTo(*this);
        return *this;
    }

    std::size_t ExecutionRuntime::cpuConcurrency() const noexcept
    {
        return state_ ? state_->workers.size() : 0;
    }

    CpuScheduler ExecutionRuntime::cpu() const noexcept
    {
        return CpuScheduler{state_};
    }

    lux::cxx::expected<std::size_t, EExecutionError> ExecutionRuntime::collectCompletions() noexcept
    {
        if (!tasks_ || state_->owner_thread != std::this_thread::get_id())
            return lux::cxx::unexpected(EExecutionError::WRONG_THREAD);
        if (state_->collecting_completions)
            return lux::cxx::unexpected(EExecutionError::INVALID_STATE);
        state_->collecting_completions = true;
        {
            std::lock_guard lock{state_->completion_mutex};
            state_->completion_batch.swap(state_->ready_completion);
        }
        auto count = tasks_->collect();
        const auto size = state_->completion_batch.size();
        for (std::size_t index{}; index < size; ++index)
        {
            const auto work = state_->completion_batch[index];
            {
                std::lock_guard lock{state_->completion_mutex};
                work->queued = false;
                if (work->cancelled)
                    continue;
                work->executing = true;
            }
            work->run(work->owner);
            work->executing = false;
            ++count;
        }
        state_->completion_batch.clear();
        state_->collecting_completions = false;
        return count;
    }

    lux::cxx::expected<std::size_t, EExecutionError> ExecutionRuntime::dispatchTaskEvents() noexcept
    {
        if (!tasks_ || state_->owner_thread != std::this_thread::get_id())
            return lux::cxx::unexpected(EExecutionError::WRONG_THREAD);
        if (tasks_->collecting || tasks_->dispatching || state_->collecting_completions)
            return lux::cxx::unexpected(EExecutionError::INVALID_STATE);
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
    void ExecutionRuntime::setTaskObserver(void* owner, TaskObserver observer) noexcept
    {
        tasks_->requireOwner();
        tasks_->observer_owner = owner;
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

    CompletionWork::CompletionWork(ExecutionRuntime& runtime, void* owner, void (*run)(void*) noexcept)
        : state_(std::make_shared<detail::CompletionWorkState>())
    {
        const auto& state = runtime.state_;
        if (!state || state->owner_thread != std::this_thread::get_id() || !owner || !run)
            std::terminate();
        std::lock_guard lock{state->completion_mutex};
        if (state->phase.load(std::memory_order_acquire) != detail::EExecutionState::ACTIVE)
            std::terminate();
        // Cancelled batch entries can still retain their node until the batch returns.
        const auto count =
            state->completion_count + state->ready_completion.size() + state->completion_batch.size() + 1;
        state->ready_completion.reserve(count);
        state->completion_batch.reserve(count);
        state_->runtime = state;
        state_->owner = owner;
        state_->run = run;
        ++state->completion_count;
    }
    CompletionWork::~CompletionWork() noexcept
    {
        cancel();
    }
    void CompletionWork::cancel() noexcept
    {
        const auto state = state_->runtime.lock();
        if (!state)
            return;
        if (state->owner_thread != std::this_thread::get_id())
            std::terminate();
        std::lock_guard lock{state->completion_mutex};
        if (state_->executing)
            std::terminate();
        if (!std::exchange(state_->cancelled, true))
            --state->completion_count;
        state_->owner = nullptr;
    }
    void CompletionWork::request() const noexcept
    {
        requester().request();
    }
    void CompletionWork::Request::request() const noexcept
    {
        const auto state = state_ ? state_->runtime.lock() : nullptr;
        if (!state)
            return;
        {
            std::lock_guard lock{state->completion_mutex};
            const bool closed =
                state_->cancelled || state->phase.load(std::memory_order_acquire) == detail::EExecutionState::JOINED;
            if (closed || state_->queued)
                return;
            state_->queued = true;
            state->ready_completion.push_back(state_);
            ++state->wake_epoch;
        }
        state->completion_ready.notify_one();
        if (const auto wake = state->wake_callback.load(std::memory_order_acquire))
            wake();
    }
    std::uint64_t ExecutionRuntime::wakeEpoch() const noexcept
    {
        std::lock_guard lock{state_->completion_mutex};
        return state_->wake_epoch;
    }
    bool ExecutionRuntime::hasPendingWork() const noexcept
    {
        if (tasks_->hasWork())
            return true;
        std::lock_guard lock{state_->completion_mutex};
        return !state_->ready_completion.empty();
    }
    void ExecutionRuntime::wake() noexcept
    {
        {
            std::lock_guard lock{state_->completion_mutex};
            ++state_->wake_epoch;
        }
        state_->completion_ready.notify_one();
        if (const auto wake = state_->wake_callback.load(std::memory_order_acquire))
            wake();
    }
    void ExecutionRuntime::setWake(void (*wake)() noexcept) noexcept
    {
        if (state_->owner_thread != std::this_thread::get_id())
            std::terminate();
        state_->wake_callback.store(wake, std::memory_order_release);
    }
    void ExecutionRuntime::waitForWork(std::uint64_t observed, std::chrono::steady_clock::time_point deadline) noexcept
    {
        if (state_->owner_thread != std::this_thread::get_id() || state_->collecting_completions)
            std::terminate();
        std::unique_lock lock{state_->completion_mutex};
        const auto ready = [&] { return state_->wake_epoch != observed || !state_->ready_completion.empty(); };
        if (deadline == std::chrono::steady_clock::time_point::max())
            state_->completion_ready.wait(lock, ready);
        else
            state_->completion_ready.wait_until(lock, deadline, ready);
    }
    lux::cxx::expected<void, EExecutionError> ExecutionRuntime::validateWait() const noexcept
    {
        if (!state_ || state_->owner_thread != std::this_thread::get_id())
            return lux::cxx::unexpected(EExecutionError::WRONG_THREAD);
        if (state_->collecting_completions)
            return lux::cxx::unexpected(EExecutionError::INVALID_STATE);
        return {};
    }
    lux::cxx::expected<void, EExecutionError> ExecutionRuntime::waitUntil(
        void* context,
        bool (*ready)(void*) noexcept
    ) noexcept
    {
        if (!state_ || state_->owner_thread != std::this_thread::get_id())
            return lux::cxx::unexpected(EExecutionError::WRONG_THREAD);
        if (ready(context))
            return {};
        if (state_->collecting_completions)
            return lux::cxx::unexpected(EExecutionError::INVALID_STATE);
        while (!ready(context))
        {
            const auto epoch = wakeEpoch();
            const auto dispatched = collectCompletions();
            if (!dispatched)
                return lux::cxx::unexpected(dispatched.error());
            if (!ready(context))
                waitForWork(epoch);
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
        if (state_->owner_thread != std::this_thread::get_id())
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
        if (!tasks_->settled())
            return lux::cxx::unexpected(EExecutionError::WORK_PENDING);
        {
            std::lock_guard lock{state_->completion_mutex};
            if (!state_->ready_completion.empty())
            {
                return lux::cxx::unexpected(EExecutionError::WORK_PENDING);
            }
        }

        timer_.requestStop();
        detail::stopState(state_);
        detail::joinWorkers(*state_);
        state_->phase.store(detail::EExecutionState::JOINED, std::memory_order_release);
        return {};
    }
} // namespace lux::process

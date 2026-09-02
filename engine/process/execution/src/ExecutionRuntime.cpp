#include <lux/engine/process/ExecutionRuntime.hpp>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <new>
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
            explicit RequestQueue(std::size_t capacity) : values(capacity)
            {
            }

            [[nodiscard]] bool push(ScheduleRequest* request) noexcept
            {
                if (count == values.size())
                    return false;
                values[tail] = request;
                tail = (tail + 1U) % values.size();
                ++count;
                return true;
            }

            [[nodiscard]] ScheduleRequest* pop() noexcept
            {
                if (count == 0U)
                    return nullptr;
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

        struct ExecutionState final
        {
            ExecutionState(ExecutionRuntimeConfig config, std::thread::id owner)
                : cpu_queue(config.cpu_queue_capacity),
                  main_queue(config.main_queue_capacity),
                  blocking_queue(config.blocking ? config.blocking->queue_capacity : 0U),
                  blocking_enabled(config.blocking.has_value()),
                  owner_thread(owner)
            {
            }

            std::atomic<EExecutionState> phase{EExecutionState::ACTIVE};
            std::mutex cpu_mutex;
            std::condition_variable cpu_ready;
            RequestQueue cpu_queue;
            std::vector<std::jthread> workers;
            std::mutex main_mutex;
            RequestQueue main_queue;
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
                            return;
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
                            return;
                    }

                    const bool stopped = stopping || request->cancel_requested.load(std::memory_order_acquire);
                    request->complete(request, stopped);
                }
            }

            void stopState(const std::shared_ptr<ExecutionState>& state) noexcept
            {
                if (!state)
                    return;
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
                        worker.join();
                }
                state.workers.clear();
                for (auto& worker : state.blocking_workers)
                {
                    if (worker.joinable())
                        worker.join();
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
                return lux::cxx::unexpected(EExecutionError::STOPPING);

            if (queue == EExecutionQueue::CPU)
            {
                std::lock_guard lock{state->cpu_mutex};
                if (state->phase.load(std::memory_order_acquire) != EExecutionState::ACTIVE)
                    return lux::cxx::unexpected(EExecutionError::STOPPING);
                if (!state->cpu_queue.push(&request))
                    return lux::cxx::unexpected(EExecutionError::CAPACITY_EXCEEDED);
                state->cpu_ready.notify_one();
                return {};
            }

            if (queue == EExecutionQueue::MAIN)
            {
                std::lock_guard lock{state->main_mutex};
                if (state->phase.load(std::memory_order_acquire) != EExecutionState::ACTIVE)
                    return lux::cxx::unexpected(EExecutionError::STOPPING);
                if (!state->main_queue.push(&request))
                    return lux::cxx::unexpected(EExecutionError::CAPACITY_EXCEEDED);
                return {};
            }

            if (!state->blocking_enabled)
                return lux::cxx::unexpected(EExecutionError::CAPABILITY_UNAVAILABLE);
            std::lock_guard lock{state->blocking_mutex};
            if (state->phase.load(std::memory_order_acquire) != EExecutionState::ACTIVE)
                return lux::cxx::unexpected(EExecutionError::STOPPING);
            if (!state->blocking_queue.push(&request))
                return lux::cxx::unexpected(EExecutionError::CAPACITY_EXCEEDED);
            state->blocking_ready.notify_one();
            return {};
        }
    } // namespace detail

    ExecutionRuntime::ExecutionRuntime(std::shared_ptr<detail::ExecutionState> state, TimerQueue timer) noexcept
        : state_(std::move(state)), timer_(std::move(timer))
    {
    }

    ExecutionRuntime::CreateResult ExecutionRuntime::create(ExecutionRuntimeConfig config) noexcept
    {
        const bool is_invalid_blocking = config.blocking &&
            (config.blocking->concurrency == 0U || config.blocking->queue_capacity == 0U);
        const bool is_invalid_config = config.cpu_concurrency == 0U || config.cpu_queue_capacity == 0U ||
            config.main_queue_capacity == 0U || config.timer.capacity == 0U || is_invalid_blocking;
        if (is_invalid_config)
            return lux::cxx::unexpected(EExecutionError::INVALID_ARGUMENT);

        auto timer = TimerQueue::create(config.timer);
        if (!timer)
        {
            const auto code = timer.error() == ETimerError::ALLOCATION_FAILURE ? EExecutionError::ALLOCATION_FAILURE :
                timer.error() == ETimerError::WORKER_CREATION_FAILURE ? EExecutionError::WORKER_CREATION_FAILURE :
                                                                         EExecutionError::BACKEND_FAILURE;
            return lux::cxx::unexpected(code);
        }

        std::shared_ptr<detail::ExecutionState> state;
        try
        {
            state = std::make_shared<detail::ExecutionState>(config, std::this_thread::get_id());
            state->workers.reserve(config.cpu_concurrency);
            if (config.blocking)
                state->blocking_workers.reserve(config.blocking->concurrency);
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EExecutionError::ALLOCATION_FAILURE);
        }

        try
        {
            for (std::size_t index{}; index < config.cpu_concurrency; ++index)
                state->workers.emplace_back([raw = state.get()] { detail::cpuWorker(*raw); });
            if (config.blocking)
            {
                for (std::size_t index{}; index < config.blocking->concurrency; ++index)
                    state->blocking_workers.emplace_back([raw = state.get()] { detail::blockingWorker(*raw); });
            }
            return ExecutionRuntime{std::move(state), std::move(*timer)};
        }
        catch (const std::bad_alloc&)
        {
            detail::stopState(state);
            detail::joinWorkers(*state);
            return lux::cxx::unexpected(EExecutionError::ALLOCATION_FAILURE);
        }
        catch (const std::system_error&)
        {
            detail::stopState(state);
            detail::joinWorkers(*state);
            return lux::cxx::unexpected(EExecutionError::WORKER_CREATION_FAILURE);
        }
        catch (...)
        {
            detail::stopState(state);
            detail::joinWorkers(*state);
            return lux::cxx::unexpected(EExecutionError::BACKEND_FAILURE);
        }
    }

    ExecutionRuntime::~ExecutionRuntime() noexcept
    {
        if (!state_)
            return;
        if (state_->owner_thread != std::this_thread::get_id())
            std::terminate();
        requestStop();
        while (true)
        {
            auto drained = drainMain();
            if (!drained || *drained == 0U)
                break;
        }
        if (auto joined = join(); !joined && joined.error() != EExecutionError::ALREADY_JOINED)
            std::terminate();
    }

    ExecutionRuntime::ExecutionRuntime(ExecutionRuntime&& other) noexcept
        : state_(std::move(other.state_)), timer_(std::move(other.timer_))
    {
    }

    ExecutionRuntime& ExecutionRuntime::operator=(ExecutionRuntime&& other) noexcept
    {
        if (this == &other)
            return *this;
        if (state_)
        {
            if (state_->owner_thread != std::this_thread::get_id())
                std::terminate();
            requestStop();
            while (true)
            {
                auto drained = drainMain();
                if (!drained || *drained == 0U)
                    break;
            }
            if (auto joined = join(); !joined && joined.error() != EExecutionError::ALREADY_JOINED)
                std::terminate();
        }
        state_ = std::move(other.state_);
        timer_ = std::move(other.timer_);
        return *this;
    }

    CpuScheduler ExecutionRuntime::cpu() const noexcept
    {
        return CpuScheduler{state_};
    }

    MainScheduler ExecutionRuntime::main() const noexcept
    {
        return MainScheduler{state_};
    }

    TimerClient ExecutionRuntime::timer() const noexcept
    {
        return timer_.client();
    }

    lux::cxx::expected<BlockingScheduler, EExecutionError> ExecutionRuntime::blocking() const noexcept
    {
        if (!state_ || !state_->blocking_enabled)
            return lux::cxx::unexpected(EExecutionError::CAPABILITY_UNAVAILABLE);
        return BlockingScheduler{state_};
    }

    lux::cxx::expected<std::size_t, EExecutionError>
    ExecutionRuntime::drainMain(std::size_t budget) noexcept
    {
        if (!state_)
            return lux::cxx::unexpected(EExecutionError::ALREADY_JOINED);
        if (state_->owner_thread != std::this_thread::get_id())
            return lux::cxx::unexpected(EExecutionError::WRONG_THREAD);
        if (state_->phase.load(std::memory_order_acquire) == detail::EExecutionState::JOINED)
            return lux::cxx::unexpected(EExecutionError::ALREADY_JOINED);

        std::size_t completed{};
        while (completed < budget)
        {
            detail::ScheduleRequest* request{};
            {
                std::lock_guard lock{state_->main_mutex};
                request = state_->main_queue.pop();
            }
            if (request == nullptr)
                break;
            const bool stopped = state_->phase.load(std::memory_order_acquire) != detail::EExecutionState::ACTIVE ||
                request->cancel_requested.load(std::memory_order_acquire);
            request->complete(request, stopped);
            ++completed;
        }
        return completed;
    }

    void ExecutionRuntime::requestStop() noexcept
    {
        if (!state_)
            return;
        detail::stopState(state_);
        timer_.requestStop();
    }

    lux::cxx::expected<void, EExecutionError> ExecutionRuntime::join() noexcept
    {
        if (!state_)
            return lux::cxx::unexpected(EExecutionError::ALREADY_JOINED);
        if (state_->owner_thread != std::this_thread::get_id())
            return lux::cxx::unexpected(EExecutionError::WRONG_THREAD);

        const auto phase = state_->phase.load(std::memory_order_acquire);
        if (phase == detail::EExecutionState::JOINED)
            return lux::cxx::unexpected(EExecutionError::ALREADY_JOINED);
        if (phase != detail::EExecutionState::STOPPING)
            return lux::cxx::unexpected(EExecutionError::INVALID_STATE);
        {
            std::lock_guard lock{state_->main_mutex};
            if (state_->main_queue.count != 0U)
                return lux::cxx::unexpected(EExecutionError::MAIN_QUEUE_NOT_DRAINED);
        }

        timer_.requestStop();
        detail::joinWorkers(*state_);
        state_->phase.store(detail::EExecutionState::JOINED, std::memory_order_release);
        return {};
    }
} // namespace lux::process

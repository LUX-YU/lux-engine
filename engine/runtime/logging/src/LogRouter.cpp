#include <lux/engine/runtime/logging/LogRouter.hpp>
#include <lux/engine/runtime/logging/LogRouterSenders.hpp>

#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScope.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>
#include <lux/engine/log/Log.hpp>

#include <exec/static_thread_pool.hpp>
#include <exec/start_detached.hpp>
#include <moodycamel/blockingconcurrentqueue.h>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <chrono>
#include <new>
#include <utility>

namespace lux::logging
{
    namespace ex = stdexec;
    using lux::exec::AsyncRuntime;
    using lux::exec::AsyncScope;
    using lux::exec::CoordinatorSignal;
    using lux::exec::MainThreadDispatcher;
    using lux::exec::blockingIoScheduler;
    using lux::exec::coordinatorScheduler;
    using lux::exec::spawn;

    namespace
    {
        void updateHighWater(
            std::atomic<std::size_t>& target,
            std::size_t value) noexcept
        {
            auto high = target.load(std::memory_order_relaxed);
            while (high < value &&
                   !target.compare_exchange_weak(
                       high,
                       value,
                       std::memory_order_relaxed,
                       std::memory_order_relaxed))
            {
            }
        }
    }

    struct LogRouter::State final :
        std::enable_shared_from_this<LogRouter::State>
    {
        State(AsyncRuntime& runtime_arg, LogRouterConfig config_arg)
            : runtime(&runtime_arg)
            , main(runtime_arg.mainThreadDispatcher())
            , scope(runtime_arg)
            , config(config_arg)
            , records(config_arg.queue_capacity)
        {}

        void enqueue(const lux::log::LogRecord& record) noexcept
        {
            if (!accepting.load(std::memory_order_acquire))
            {
                lux::log::writeRecordToStderr(record);
                return;
            }

            auto depth = queued.load(std::memory_order_relaxed);
            for (;;)
            {
                if (depth >= config.queue_capacity)
                {
                    dropped.fetch_add(1u, std::memory_order_relaxed);
                    return;
                }
                if (queued.compare_exchange_weak(
                        depth,
                        depth + 1u,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed))
                    break;
            }

            if (!records.try_enqueue(record))
            {
                queued.fetch_sub(1u, std::memory_order_release);
                dropped.fetch_add(1u, std::memory_order_relaxed);
                return;
            }
            accepted.fetch_add(1u, std::memory_order_relaxed);
            updateHighWater(queue_high_water, depth + 1u);
            schedule();
        }

        void schedule() noexcept
        {
            if (drain_scheduled.exchange(true, std::memory_order_acq_rel))
                return;

            if (!drain_signal.notify())
            {
                drain_scheduled.store(false, std::memory_order_release);
                // The runtime is already closing. Preserve diagnostics rather
                // than parking records behind a dead scheduler.
                drainSynchronously();
            }
        }

        void startBatch() noexcept
        {
            auto self = shared_from_this();
            auto work = ex::schedule(blockingIoScheduler(*runtime))
                | ex::then(
                      [self]() noexcept
                      {
                          self->writeBatch();
                      })
                | ex::continues_on(
                      coordinatorScheduler(*runtime))
                | ex::then(
                      [self]() noexcept
                      {
                          self->finishBatch();
                      })
                | ex::upon_stopped(
                      [self]() noexcept
                      {
                          self->drain_scheduled.store(
                              false,
                              std::memory_order_release);
                          self->signalProgress();
                      });
            if (!spawn(scope, std::move(work)))
            {
                drain_scheduled.store(false, std::memory_order_release);
                signalProgress();
            }
        }

        void writeBatch() noexcept
        {
            const std::size_t limit = std::max<std::size_t>(
                1u,
                config.batch_size);
            lux::log::LogRecord record;
            std::size_t count = 0;
            while (count < limit && records.try_dequeue(record))
            {
                queued.fetch_sub(1u, std::memory_order_release);
                lux::log::writeRecordToStderr(record);
                written.fetch_add(1u, std::memory_order_relaxed);

                if (record.level == lux::log::ELevel::Error && toast)
                {
                    auto self = shared_from_this();
                    const auto copy = record;
                    if (!main.tryDispatchToMainThread(
                            [self = std::move(self), copy]() noexcept
                            {
                                if (self->toast)
                                    self->toast(copy);
                            }))
                    {
                        dropped_toasts.fetch_add(
                            1u,
                            std::memory_order_relaxed);
                    }
                }
                ++count;
            }
            signalProgress();
        }

        void finishBatch() noexcept
        {
            if (queued.load(std::memory_order_acquire) != 0u)
            {
                startBatch();
                return;
            }

            drain_scheduled.store(false, std::memory_order_release);
            signalProgress();
            // Close the empty-observation race with a producer that enqueued
            // immediately before the gate was cleared.
            if (queued.load(std::memory_order_acquire) != 0u)
                schedule();
            else
                tryFinishClose();
        }

        void drainSynchronously() noexcept
        {
            lux::log::LogRecord record;
            while (records.try_dequeue(record))
            {
                queued.fetch_sub(1u, std::memory_order_release);
                lux::log::writeRecordToStderr(record);
                written.fetch_add(1u, std::memory_order_relaxed);
            }
            signalProgress();
            tryFinishClose();
        }

        void signalProgress() noexcept
        {
            progress_epoch.fetch_add(1u, std::memory_order_release);
            progress_epoch.notify_all();
        }

        struct CloseWaiter final
        {
            explicit CloseWaiter(
                lux::cxx::move_only_function<void(LogRouterStatistics)> value)
                noexcept
                : completion(std::move(value))
            {}

            CloseWaiter* next{nullptr};
            lux::cxx::move_only_function<void(LogRouterStatistics)> completion;
        };

        [[nodiscard]] static CloseWaiter* completedSentinel() noexcept
        {
            return reinterpret_cast<CloseWaiter*>(std::uintptr_t{1u});
        }

        [[nodiscard]] LogRouterStatistics statistics() const noexcept
        {
            return LogRouterStatistics{
                .accepted = accepted.load(std::memory_order_relaxed),
                .written = written.load(std::memory_order_relaxed),
                .dropped = dropped.load(std::memory_order_relaxed),
                .dropped_toasts = dropped_toasts.load(
                    std::memory_order_relaxed),
                .flush_latency_ns = flush_latency_ns.load(
                    std::memory_order_relaxed),
                .queue_high_water = queue_high_water.load(
                    std::memory_order_relaxed)};
        }

        void subscribeClose(
            lux::cxx::move_only_function<void(LogRouterStatistics)> completion)
            noexcept
        {
            auto* waiter = new (std::nothrow) CloseWaiter{
                std::move(completion)};
            if (waiter == nullptr)
                std::terminate();
            auto* head = close_waiters.load(std::memory_order_acquire);
            for (;;)
            {
                if (head == completedSentinel())
                {
                    auto terminal = std::move(waiter->completion);
                    delete waiter;
                    terminal(statistics());
                    return;
                }
                waiter->next = head;
                if (close_waiters.compare_exchange_weak(
                        head,
                        waiter,
                        std::memory_order_release,
                        std::memory_order_acquire))
                    break;
            }

            if (closing.exchange(true, std::memory_order_acq_rel))
                return;
            accepting.store(false, std::memory_order_release);
            if (installed.exchange(false, std::memory_order_acq_rel))
                lux::log::setOutput({});
            flush_started = std::chrono::steady_clock::now();
            schedule();
            tryFinishClose();
        }

        void tryFinishClose() noexcept
        {
            if (!closing.load(std::memory_order_acquire) ||
                queued.load(std::memory_order_acquire) != 0u ||
                drain_scheduled.load(std::memory_order_acquire) ||
                scope_close_started.exchange(true, std::memory_order_acq_rel))
                return;

            auto self = shared_from_this();
            auto close = scope.closeAsync()
                | ex::then(
                      [self = std::move(self)]() noexcept
                      {
                          self->finishClose();
                      });
            ::experimental::execution::start_detached(std::move(close));
        }

        void finishClose() noexcept
        {
            flush_latency_ns.store(
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - flush_started)
                        .count()),
                std::memory_order_relaxed);
            auto result = statistics();
            auto* waiter = close_waiters.exchange(
                completedSentinel(), std::memory_order_acq_rel);
            while (waiter != nullptr && waiter != completedSentinel())
            {
                auto* next = waiter->next;
                auto terminal = std::move(waiter->completion);
                delete waiter;
                terminal(result);
                waiter = next;
            }
        }

        AsyncRuntime* runtime{nullptr};
        MainThreadDispatcher main;
        CoordinatorSignal drain_signal;
        AsyncScope scope;
        LogRouterConfig config;
        moodycamel::BlockingConcurrentQueue<lux::log::LogRecord> records;
        ToastSink toast;
        std::atomic<bool> accepting{true};
        std::atomic<bool> drain_scheduled{false};
        std::atomic<std::size_t> queued{0};
        std::atomic<std::size_t> queue_high_water{0};
        std::atomic<std::uint64_t> accepted{0};
        std::atomic<std::uint64_t> written{0};
        std::atomic<std::uint64_t> dropped{0};
        std::atomic<std::uint64_t> dropped_toasts{0};
        std::atomic<std::uint64_t> progress_epoch{0};
        std::atomic<std::uint64_t> flush_latency_ns{0};
        std::atomic<bool> installed{false};
        std::atomic<bool> closing{false};
        std::atomic<bool> scope_close_started{false};
        std::atomic<CloseWaiter*> close_waiters{nullptr};
        std::chrono::steady_clock::time_point flush_started{};
    };

    LogRouter::LogRouter(
        AsyncRuntime& runtime,
        LogRouterConfig config)
        : state_(std::make_shared<State>(runtime, config))
    {
        std::weak_ptr<State> weak = state_;
        state_->drain_signal = runtime.makeCoordinatorSignal(
            [weak]() noexcept
            {
                if (auto state = weak.lock())
                    state->startBatch();
            });
    }

    LogRouter::~LogRouter()
    {
        if (state_)
            state_->accepting.store(false, std::memory_order_release);
    }

    void LogRouter::install(ToastSink toast)
    {
        if (state_->closing.load(std::memory_order_acquire) ||
            state_->installed.exchange(true, std::memory_order_acq_rel))
            return;
        state_->toast = std::move(toast);
        std::weak_ptr<State> weak = state_;
        lux::log::setOutput(
            [weak = std::move(weak)](
                const lux::log::LogRecord& record) noexcept
            {
                if (auto state = weak.lock())
                    state->enqueue(record);
                else
                    lux::log::writeRecordToStderr(record);
            });
    }

    LogRouterCloseSender LogRouter::closeAsync() noexcept
    {
        return LogRouterCloseSender{
            state_,
            [](std::shared_ptr<void> opaque,
               lux::cxx::move_only_function<void(LogRouterStatistics)>
                   completion) noexcept
            {
                std::static_pointer_cast<State>(std::move(opaque))
                    ->subscribeClose(std::move(completion));
            }};
    }

    LogRouterStatistics LogRouter::statistics() const noexcept
    {
        return state_->statistics();
    }

    void detail::subscribeLogRouterClose(
        LogRouter& router,
        lux::cxx::move_only_function<void(LogRouterStatistics)> completion)
        noexcept
    {
        router.state_->subscribeClose(std::move(completion));
    }
}

#include <lux/engine/process/Timer.hpp>

#include <lux/engine/process/detail/TimerFailureInjection.hpp>

#include <stdexec/execution.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace
{
    using namespace std::chrono_literals;

    enum class ETerminal : std::uint8_t
    {
        NONE,
        VALUE,
        ERROR,
        STOPPED
    };

    struct Outcome final
    {
        void finish(ETerminal value) noexcept
        {
            terminal.store(value, std::memory_order_release);
            completions.fetch_add(1U, std::memory_order_acq_rel);
            completions.notify_all();
        }

        void wait() noexcept
        {
            auto observed = completions.load(std::memory_order_acquire);
            while (observed == 0U)
            {
                completions.wait(observed, std::memory_order_acquire);
                observed = completions.load(std::memory_order_acquire);
            }
        }

        std::atomic<std::size_t> completions{};
        std::atomic<ETerminal> terminal{ETerminal::NONE};
        std::atomic<lux::process::ETimerError> error{lux::process::ETimerError::BACKEND_FAILURE};
    };

    struct Receiver final
    {
        using receiver_concept = stdexec::receiver_t;

        void set_value() && noexcept
        {
            outcome->finish(ETerminal::VALUE);
        }

        void set_error(lux::process::ETimerError value) && noexcept
        {
            outcome->error.store(value, std::memory_order_release);
            outcome->finish(ETerminal::ERROR);
        }

        void set_stopped() && noexcept
        {
            outcome->finish(ETerminal::STOPPED);
        }

        [[nodiscard]] auto get_env() const noexcept
        {
            return stdexec::prop{stdexec::get_stop_token, stop_token};
        }

        Outcome* outcome{};
        stdexec::inplace_stop_token stop_token;
    };

    struct OrderedReceiver final
    {
        using receiver_concept = stdexec::receiver_t;

        void set_value() && noexcept
        {
            {
                std::lock_guard lock{*mutex};
                order->push_back(id);
            }
            outcome->finish(ETerminal::VALUE);
        }

        void set_error(lux::process::ETimerError value) && noexcept
        {
            outcome->error.store(value, std::memory_order_release);
            outcome->finish(ETerminal::ERROR);
        }

        void set_stopped() && noexcept
        {
            outcome->finish(ETerminal::STOPPED);
        }

        [[nodiscard]] stdexec::empty_env get_env() const noexcept
        {
            return {};
        }

        Outcome* outcome{};
        std::mutex* mutex{};
        std::vector<int>* order{};
        int id{};
    };

    void waitFor(std::atomic<std::size_t>& value, std::size_t target) noexcept
    {
        auto observed = value.load(std::memory_order_acquire);
        while (observed < target)
        {
            value.wait(observed, std::memory_order_acquire);
            observed = value.load(std::memory_order_acquire);
        }
    }

    void testFactoryFailures()
    {
        const auto invalid = lux::process::TimerQueue::create({});
        assert(!invalid);
        assert(invalid.error() == lux::process::ETimerError::INVALID_ARGUMENT);

        using lux::process::detail::testing::ETimerCreateFailure;
        for (const auto [injected, expected] : std::array{
                 std::pair{ETimerCreateFailure::ALLOCATION, lux::process::ETimerError::ALLOCATION_FAILURE},
                 std::pair{ETimerCreateFailure::WORKER, lux::process::ETimerError::WORKER_CREATION_FAILURE},
                 std::pair{ETimerCreateFailure::BACKEND, lux::process::ETimerError::BACKEND_FAILURE}
             })
        {
            lux::process::detail::testing::failNextTimerCreate(injected);
            const auto result = lux::process::TimerQueue::create({1U});
            assert(!result);
            assert(result.error() == expected);
        }
    }

    void testLazyStartAndImmediateExpiry()
    {
        auto created = lux::process::TimerQueue::create({4U});
        assert(created);
        auto timer = std::move(*created);

        stdexec::inplace_stop_source lazy_stop;
        Outcome lazy;
        auto lazy_operation = stdexec::connect(timer.client().after(15ms), Receiver{&lazy, lazy_stop.get_token()});
        std::this_thread::sleep_for(20ms);
        assert(lazy.completions.load(std::memory_order_acquire) == 0U);
        const auto started = std::chrono::steady_clock::now();
        stdexec::start(lazy_operation);
        lazy.wait();
        assert(lazy.terminal.load(std::memory_order_acquire) == ETerminal::VALUE);
        assert(std::chrono::steady_clock::now() - started >= 5ms);

        stdexec::inplace_stop_source immediate_stop;
        Outcome immediate;
        auto immediate_operation = stdexec::connect(
            timer.client().after(std::chrono::steady_clock::duration::zero()),
            Receiver{&immediate, immediate_stop.get_token()}
        );
        stdexec::start(immediate_operation);
        immediate.wait();
        assert(immediate.terminal.load(std::memory_order_acquire) == ETerminal::VALUE);
    }

    void testCancellationCapacityAndShutdown()
    {
        auto created = lux::process::TimerQueue::create({1U});
        assert(created);
        auto timer = std::move(*created);

        stdexec::inplace_stop_source first_stop;
        Outcome first;
        auto first_operation = stdexec::connect(timer.client().after(1h), Receiver{&first, first_stop.get_token()});
        stdexec::start(first_operation);

        stdexec::inplace_stop_source overflow_stop;
        Outcome overflow;
        auto overflow_operation = stdexec::connect(
            timer.client().after(1h),
            Receiver{&overflow, overflow_stop.get_token()}
        );
        stdexec::start(overflow_operation);
        overflow.wait();
        assert(overflow.terminal.load(std::memory_order_acquire) == ETerminal::ERROR);
        assert(overflow.error.load(std::memory_order_acquire) == lux::process::ETimerError::CAPACITY_EXCEEDED);

        static_cast<void>(first_stop.request_stop());
        first.wait();
        assert(first.terminal.load(std::memory_order_acquire) == ETerminal::STOPPED);

        stdexec::inplace_stop_source live_stop;
        Outcome live;
        auto live_operation = stdexec::connect(timer.client().after(1h), Receiver{&live, live_stop.get_token()});
        stdexec::start(live_operation);
        const auto stale = timer.client();
        timer.requestStop();
        live.wait();
        assert(live.terminal.load(std::memory_order_acquire) == ETerminal::STOPPED);

        stdexec::inplace_stop_source stale_stop;
        Outcome stale_outcome;
        auto stale_operation = stdexec::connect(stale.after(1ms), Receiver{&stale_outcome, stale_stop.get_token()});
        stdexec::start(stale_operation);
        stale_outcome.wait();
        assert(stale_outcome.terminal.load(std::memory_order_acquire) == ETerminal::ERROR);
        assert(stale_outcome.error.load(std::memory_order_acquire) == lux::process::ETimerError::STOPPING);
        assert(first.completions.load(std::memory_order_acquire) == 1U);
    }

    void testDeadlineOrder()
    {
        auto created = lux::process::TimerQueue::create({3U});
        assert(created);
        auto timer = std::move(*created);
        std::mutex mutex;
        std::vector<int> order;
        order.reserve(3U);
        Outcome slow;
        Outcome fast;
        Outcome middle;
        auto slow_operation = stdexec::connect(
            timer.client().after(30ms),
            OrderedReceiver{&slow, &mutex, &order, 3}
        );
        auto fast_operation = stdexec::connect(
            timer.client().after(5ms),
            OrderedReceiver{&fast, &mutex, &order, 1}
        );
        auto middle_operation = stdexec::connect(
            timer.client().after(15ms),
            OrderedReceiver{&middle, &mutex, &order, 2}
        );
        stdexec::start(slow_operation);
        stdexec::start(fast_operation);
        stdexec::start(middle_operation);
        slow.wait();
        fast.wait();
        middle.wait();
        assert((order == std::vector{1, 2, 3}));
    }

    void testPreStartCancellationAndOwnerMove()
    {
        auto created = lux::process::TimerQueue::create({2U});
        assert(created);
        auto original = std::move(*created);
        auto moved = std::move(original);

        stdexec::inplace_stop_source stop;
        Outcome outcome;
        auto operation = stdexec::connect(moved.client().after(1s), Receiver{&outcome, stop.get_token()});
        static_cast<void>(stop.request_stop());
        stdexec::start(operation);
        outcome.wait();
        assert(outcome.terminal.load(std::memory_order_acquire) == ETerminal::STOPPED);
        assert(outcome.completions.load(std::memory_order_acquire) == 1U);
    }

    void testConcurrentProducersAndRace()
    {
        constexpr std::size_t kProducerCount = 8U;
        constexpr std::size_t kEpochCount = 50U;
        auto created = lux::process::TimerQueue::create({kProducerCount});
        assert(created);
        auto timer = std::move(*created);
        std::atomic<std::size_t> ready{};
        std::atomic<std::size_t> go{};
        std::atomic<std::size_t> completed{};

        std::vector<std::jthread> producers;
        producers.reserve(kProducerCount);
        for (std::size_t producer = 0U; producer < kProducerCount; ++producer)
        {
            producers.emplace_back([&, producer]() noexcept {
                static_cast<void>(producer);
                for (std::size_t epoch = 0U; epoch < kEpochCount; ++epoch)
                {
                    stdexec::inplace_stop_source stop;
                    Outcome outcome;
                    auto operation = stdexec::connect(timer.client().after(0ns), Receiver{&outcome, stop.get_token()});
                    ready.fetch_add(1U, std::memory_order_acq_rel);
                    ready.notify_all();
                    waitFor(go, (epoch + 1U) * kProducerCount);
                    stdexec::start(operation);
                    outcome.wait();
                    assert(outcome.terminal.load(std::memory_order_acquire) == ETerminal::VALUE);
                    assert(outcome.completions.load(std::memory_order_acquire) == 1U);
                    completed.fetch_add(1U, std::memory_order_acq_rel);
                    completed.notify_all();
                }
            });
        }

        for (std::size_t epoch = 0U; epoch < kEpochCount; ++epoch)
        {
            const auto target = (epoch + 1U) * kProducerCount;
            waitFor(ready, target);
            go.store(target, std::memory_order_release);
            go.notify_all();
            waitFor(completed, target);
        }
        producers.clear();
        assert(completed.load(std::memory_order_acquire) == kProducerCount * kEpochCount);

        for (std::size_t round = 0U; round < 100U; ++round)
        {
            stdexec::inplace_stop_source stop;
            Outcome outcome;
            auto operation = stdexec::connect(timer.client().after(100us), Receiver{&outcome, stop.get_token()});
            stdexec::start(operation);
            std::jthread canceller([&stop]() noexcept {
                static_cast<void>(stop.request_stop());
            });
            outcome.wait();
            assert(outcome.completions.load(std::memory_order_acquire) == 1U);
            const auto terminal = outcome.terminal.load(std::memory_order_acquire);
            assert(terminal == ETerminal::VALUE || terminal == ETerminal::STOPPED);
        }
    }
}

int main()
{
    static_assert(stdexec::sender<lux::process::TimerSender>);
    static_assert(!std::is_move_constructible_v<lux::process::TimerSender::Operation<Receiver>>);

    testFactoryFailures();
    testLazyStartAndImmediateExpiry();
    testCancellationCapacityAndShutdown();
    testDeadlineOrder();
    testPreStartCancellationAndOwnerMove();
    testConcurrentProducersAndRace();
    return 0;
}

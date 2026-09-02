#ifdef NDEBUG
#undef NDEBUG
#endif

#include <lux/engine/process/ExecutionRuntime.hpp>

#include <stdexec/execution.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <thread>

namespace
{
    enum class ETerminal : std::uint8_t
    {
        NONE,
        VALUE,
        ERROR,
        STOPPED,
    };

    struct Outcome final
    {
        void finish(ETerminal value) noexcept
        {
            terminal.store(value, std::memory_order_release);
            terminal.notify_all();
        }

        void wait() noexcept
        {
            auto value = terminal.load(std::memory_order_acquire);
            while (value == ETerminal::NONE)
            {
                terminal.wait(value, std::memory_order_acquire);
                value = terminal.load(std::memory_order_acquire);
            }
        }

        std::atomic<ETerminal> terminal{ETerminal::NONE};
        std::atomic<lux::process::EExecutionError> error{lux::process::EExecutionError::BACKEND_FAILURE};
        std::thread::id completion_thread;
    };

    struct Receiver final
    {
        using receiver_concept = stdexec::receiver_t;

        void set_value() && noexcept
        {
            outcome->completion_thread = std::this_thread::get_id();
            outcome->finish(ETerminal::VALUE);
        }

        void set_error(lux::process::EExecutionError value) && noexcept
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

    [[nodiscard]] lux::process::ExecutionRuntime makeRuntime(
        std::size_t concurrency = 2U,
        std::size_t cpu_capacity = 8U,
        std::size_t main_capacity = 8U
    )
    {
        auto created = lux::process::ExecutionRuntime::create({
            concurrency,
            cpu_capacity,
            main_capacity,
            {8U}
        });
        assert(created);
        return std::move(*created);
    }

    void updateMaximum(std::atomic<int>& maximum, int value) noexcept
    {
        auto observed = maximum.load(std::memory_order_relaxed);
        while (observed < value &&
               !maximum.compare_exchange_weak(observed, value, std::memory_order_relaxed))
        {
        }
    }

    void testFactoryAndParallelCpu()
    {
        assert(!lux::process::ExecutionRuntime::create({}));
        assert(!lux::process::ExecutionRuntime::create({1U, 0U, 1U, {1U}}));
        assert(!lux::process::ExecutionRuntime::create({1U, 1U, 0U, {1U}}));
        assert(!lux::process::ExecutionRuntime::create({1U, 1U, 1U, {0U}}));

        auto runtime = makeRuntime(4U, 8U, 8U);
        std::atomic<int> active{};
        std::atomic<int> maximum{};
        std::atomic<int> entered{};
        std::atomic_bool release{};
        const auto task = [&]() noexcept {
            const auto current = active.fetch_add(1, std::memory_order_acq_rel) + 1;
            updateMaximum(maximum, current);
            entered.fetch_add(1, std::memory_order_acq_rel);
            entered.notify_all();
            release.wait(false, std::memory_order_acquire);
            active.fetch_sub(1, std::memory_order_acq_rel);
        };

        std::array<Outcome, 4U> outcomes;
        std::array<stdexec::inplace_stop_source, 4U> stops;
        auto sender = stdexec::schedule(runtime.cpu()) | stdexec::then(task);
        auto first = stdexec::connect(sender, Receiver{&outcomes[0], stops[0].get_token()});
        auto second = stdexec::connect(sender, Receiver{&outcomes[1], stops[1].get_token()});
        auto third = stdexec::connect(sender, Receiver{&outcomes[2], stops[2].get_token()});
        auto fourth = stdexec::connect(sender, Receiver{&outcomes[3], stops[3].get_token()});
        stdexec::start(first);
        stdexec::start(second);
        stdexec::start(third);
        stdexec::start(fourth);

        auto observed = entered.load(std::memory_order_acquire);
        while (observed < 4)
        {
            entered.wait(observed, std::memory_order_acquire);
            observed = entered.load(std::memory_order_acquire);
        }
        assert(maximum.load(std::memory_order_acquire) > 1);
        release.store(true, std::memory_order_release);
        release.notify_all();
        for (auto& outcome : outcomes)
        {
            outcome.wait();
            assert(outcome.terminal.load(std::memory_order_acquire) == ETerminal::VALUE);
        }

        runtime.requestStop();
        assert(runtime.join());
    }

    void testBoundedCpuAdmission()
    {
        auto runtime = makeRuntime(1U, 1U, 2U);
        std::atomic_bool entered{};
        std::atomic_bool release{};
        auto blocking = stdexec::schedule(runtime.cpu()) | stdexec::then([&]() noexcept {
            entered.store(true, std::memory_order_release);
            entered.notify_all();
            release.wait(false, std::memory_order_acquire);
        });
        Outcome first_outcome;
        stdexec::inplace_stop_source first_stop;
        auto first = stdexec::connect(blocking, Receiver{&first_outcome, first_stop.get_token()});
        stdexec::start(first);
        entered.wait(false, std::memory_order_acquire);

        Outcome queued_outcome;
        stdexec::inplace_stop_source queued_stop;
        auto queued = stdexec::connect(
            stdexec::schedule(runtime.cpu()),
            Receiver{&queued_outcome, queued_stop.get_token()}
        );
        stdexec::start(queued);

        Outcome rejected_outcome;
        stdexec::inplace_stop_source rejected_stop;
        auto rejected = stdexec::connect(
            stdexec::schedule(runtime.cpu()),
            Receiver{&rejected_outcome, rejected_stop.get_token()}
        );
        stdexec::start(rejected);
        rejected_outcome.wait();
        assert(rejected_outcome.terminal.load(std::memory_order_acquire) == ETerminal::ERROR);
        assert(
            rejected_outcome.error.load(std::memory_order_acquire) ==
            lux::process::EExecutionError::CAPACITY_EXCEEDED
        );

        runtime.requestStop();
        release.store(true, std::memory_order_release);
        release.notify_all();
        first_outcome.wait();
        queued_outcome.wait();
        assert(queued_outcome.terminal.load(std::memory_order_acquire) == ETerminal::STOPPED);
        assert(runtime.join());
    }

    void testMainMailboxAndOwnerThread()
    {
        auto runtime = makeRuntime(1U, 2U, 1U);
        const auto owner = std::this_thread::get_id();
        Outcome first_outcome;
        stdexec::inplace_stop_source first_stop;
        auto first = stdexec::connect(
            stdexec::schedule(runtime.main()),
            Receiver{&first_outcome, first_stop.get_token()}
        );
        stdexec::start(first);
        assert(first_outcome.terminal.load(std::memory_order_acquire) == ETerminal::NONE);

        Outcome rejected_outcome;
        stdexec::inplace_stop_source rejected_stop;
        auto rejected = stdexec::connect(
            stdexec::schedule(runtime.main()),
            Receiver{&rejected_outcome, rejected_stop.get_token()}
        );
        stdexec::start(rejected);
        rejected_outcome.wait();
        assert(rejected_outcome.terminal.load(std::memory_order_acquire) == ETerminal::ERROR);
        assert(
            rejected_outcome.error.load(std::memory_order_acquire) ==
            lux::process::EExecutionError::CAPACITY_EXCEEDED
        );

        std::atomic<lux::process::EExecutionError> wrong_thread{};
        std::jthread foreign([&] {
            const auto result = runtime.drainMain();
            assert(!result);
            wrong_thread.store(result.error(), std::memory_order_release);
        });
        foreign.join();
        assert(wrong_thread.load(std::memory_order_acquire) == lux::process::EExecutionError::WRONG_THREAD);

        const auto drained = runtime.drainMain(1U);
        assert(drained && *drained == 1U);
        first_outcome.wait();
        assert(first_outcome.completion_thread == owner);

        assert(!runtime.join());
        runtime.requestStop();
        assert(runtime.join());
        assert(!runtime.join());

        Outcome stale_outcome;
        stdexec::inplace_stop_source stale_stop;
        auto stale = stdexec::connect(
            stdexec::schedule(runtime.main()),
            Receiver{&stale_outcome, stale_stop.get_token()}
        );
        stdexec::start(stale);
        stale_outcome.wait();
        assert(stale_outcome.terminal.load(std::memory_order_acquire) == ETerminal::ERROR);
        assert(stale_outcome.error.load(std::memory_order_acquire) == lux::process::EExecutionError::STOPPING);
    }

    void testStopBeforeStartAndMainDrainGate()
    {
        auto runtime = makeRuntime(1U, 2U, 2U);
        const auto cpu = runtime.cpu();
        runtime.requestStop();

        Outcome stopped_outcome;
        stdexec::inplace_stop_source stop;
        auto stopped = stdexec::connect(stdexec::schedule(cpu), Receiver{&stopped_outcome, stop.get_token()});
        stdexec::start(stopped);
        stopped_outcome.wait();
        assert(stopped_outcome.terminal.load(std::memory_order_acquire) == ETerminal::ERROR);
        assert(stopped_outcome.error.load(std::memory_order_acquire) == lux::process::EExecutionError::STOPPING);
        assert(runtime.join());

        auto with_main = makeRuntime(1U, 2U, 2U);
        Outcome main_outcome;
        stdexec::inplace_stop_source main_stop;
        auto main = stdexec::connect(
            stdexec::schedule(with_main.main()),
            Receiver{&main_outcome, main_stop.get_token()}
        );
        stdexec::start(main);
        with_main.requestStop();
        const auto early_join = with_main.join();
        assert(!early_join);
        assert(early_join.error() == lux::process::EExecutionError::MAIN_QUEUE_NOT_DRAINED);
        assert(with_main.drainMain());
        main_outcome.wait();
        assert(main_outcome.terminal.load(std::memory_order_acquire) == ETerminal::STOPPED);
        assert(with_main.join());
    }
}

int main()
{
    testFactoryAndParallelCpu();
    testBoundedCpuAdmission();
    testMainMailboxAndOwnerThread();
    testStopBeforeStartAndMainDrainGate();
    return 0;
}

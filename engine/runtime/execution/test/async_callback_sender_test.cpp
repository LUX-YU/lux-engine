// callbackSender stop/lifetime protocol acceptance test (CPU only).

#include <lux/engine/runtime/execution/AsyncCallbackSender.hpp>

#include <lux/cxx/core/move_only_function.hpp>

#include <stdexec/execution.hpp>

#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <utility>

namespace ex = stdexec;

namespace
{
    enum class ETerminal : int
    {
        None,
        Value,
        Error,
        Stopped,
    };

    struct Outcome
    {
        std::atomic<int> completions{0};
        std::atomic<ETerminal> terminal{ETerminal::None};
        std::atomic<int> value{0};
    };

    struct Receiver
    {
        using receiver_concept = ex::receiver_t;

        Outcome*               outcome{nullptr};
        ex::inplace_stop_token stop_token;

        void set_value(int value) && noexcept
        {
            outcome->value.store(value, std::memory_order_relaxed);
            outcome->terminal.store(ETerminal::Value, std::memory_order_release);
            outcome->completions.fetch_add(1, std::memory_order_acq_rel);
        }

        void set_error(lux::exec::AsyncCallbackError) && noexcept
        {
            outcome->terminal.store(ETerminal::Error, std::memory_order_release);
            outcome->completions.fetch_add(1, std::memory_order_acq_rel);
        }

        void set_stopped() && noexcept
        {
            outcome->terminal.store(
                ETerminal::Stopped,
                std::memory_order_release
            );
            outcome->completions.fetch_add(1, std::memory_order_acq_rel);
        }

        [[nodiscard]] auto get_env() const noexcept
        {
            return ex::prop{ex::get_stop_token, stop_token};
        }
    };
}

int main()
{
    int failures = 0;
    auto check = [&](bool condition, const char* message)
    {
        if (condition)
            std::printf("[ ok ] %s\n", message);
        else
        {
            std::printf("[FAIL] %s\n", message);
            ++failures;
        }
    };

    {
        ex::inplace_stop_source stop;
        Outcome outcome;
        auto sender = lux::exec::callbackSender<int>(
            [](auto complete) noexcept -> lux::exec::AsyncStopAction
            {
                std::move(complete).complete(17);
                return {};
            }
        );
        auto op = ex::connect(
            std::move(sender),
            Receiver{&outcome, stop.get_token()}
        );
        ex::start(op);
        check(outcome.completions.load() == 1
                  && outcome.terminal.load() == ETerminal::Value
                  && outcome.value.load() == 17,
              "synchronous value completes exactly once");
    }

    {
        ex::inplace_stop_source stop;
        Outcome outcome;
        auto sender = lux::exec::callbackSender<int>(
            [](auto complete) noexcept -> lux::exec::AsyncStopAction
            {
                std::move(complete).fail("expected failure");
                return {};
            }
        );
        auto op = ex::connect(
            std::move(sender),
            Receiver{&outcome, stop.get_token()}
        );
        ex::start(op);
        check(outcome.completions.load() == 1
                  && outcome.terminal.load() == ETerminal::Error,
              "synchronous error completes exactly once");
    }

    {
        ex::inplace_stop_source stop;
        Outcome outcome;
        int init_calls = 0;
        auto sender = lux::exec::callbackSender<int>(
            [&init_calls](auto) noexcept -> lux::exec::AsyncStopAction
            {
                ++init_calls;
                return {};
            }
        );
        auto op = ex::connect(
            std::move(sender),
            Receiver{&outcome, stop.get_token()}
        );
        (void)stop.request_stop();
        ex::start(op);
        check(init_calls == 0,
              "stop-before-start does not launch the external operation");
        check(outcome.completions.load() == 1
                  && outcome.terminal.load() == ETerminal::Stopped,
              "stop-before-start produces one stopped terminal");
    }

    {
        ex::inplace_stop_source stop;
        Outcome outcome;
        int stop_actions = 0;
        auto sender = lux::exec::callbackSender<int>(
            [&stop, &stop_actions](auto) noexcept
                -> lux::exec::AsyncStopAction
            {
                // Exercises stop while the init function has not yet returned
                // its ownership handoff. set_stopped must be deferred until it
                // does return.
                (void)stop.request_stop();
                return lux::exec::AsyncStopAction{
                    [&stop_actions]() noexcept { ++stop_actions; }
                };
            }
        );
        auto op = ex::connect(
            std::move(sender),
            Receiver{&outcome, stop.get_token()}
        );
        ex::start(op);
        check(stop_actions == 1,
              "stop during init runs the returned stop action exactly once");
        check(outcome.completions.load() == 1
                  && outcome.terminal.load() == ETerminal::Stopped,
              "stop during init completes only after handoff");
    }

    {
        ex::inplace_stop_source stop;
        Outcome outcome;
        int stop_actions = 0;
        lux::cxx::move_only_function<void()> late_value;

        auto sender = lux::exec::callbackSender<int>(
            [&late_value, &stop_actions](auto complete) mutable noexcept
                -> lux::exec::AsyncStopAction
            {
                late_value =
                    [complete = std::move(complete)]() mutable noexcept
                    { std::move(complete).complete(99); };
                // This action models a handoff to a longer-lived reaper. It
                // intentionally keeps the late callback alive.
                return lux::exec::AsyncStopAction{
                    [&stop_actions]() noexcept { ++stop_actions; }
                };
            }
        );
        auto op = ex::connect(
            std::move(sender),
            Receiver{&outcome, stop.get_token()}
        );
        ex::start(op);
        (void)stop.request_stop();
        (void)stop.request_stop();
        check(stop_actions == 1,
              "repeated stop invokes the handoff action once");
        check(outcome.completions.load() == 1
                  && outcome.terminal.load() == ETerminal::Stopped,
              "an in-flight never-callback operation becomes stopped");

        late_value();
        check(outcome.completions.load() == 1
                  && outcome.terminal.load() == ETerminal::Stopped,
              "a late value hits shared state without a second terminal");
    }

    constexpr int kRaceRounds = 1'000;
    int bad_races = 0;
    for (int round = 0; round < kRaceRounds; ++round)
    {
        ex::inplace_stop_source stop;
        Outcome outcome;
        std::atomic<int> stop_actions{0};
        auto late_value =
            std::make_shared<lux::cxx::move_only_function<void()>>();

        auto sender = lux::exec::callbackSender<int>(
            [late_value, &stop_actions](auto complete) mutable noexcept
                -> lux::exec::AsyncStopAction
            {
                *late_value =
                    [complete = std::move(complete)]() mutable noexcept
                    { std::move(complete).complete(5); };
                return lux::exec::AsyncStopAction{
                    [&stop_actions]() noexcept
                    { stop_actions.fetch_add(1, std::memory_order_relaxed); }
                };
            }
        );
        auto op = ex::connect(
            std::move(sender),
            Receiver{&outcome, stop.get_token()}
        );
        ex::start(op);

        std::atomic<bool> go{false};
        std::thread stopper([&]() noexcept
        {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            (void)stop.request_stop();
        });
        std::thread completer([&]() noexcept
        {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            (*late_value)();
        });
        go.store(true, std::memory_order_release);
        stopper.join();
        completer.join();

        const auto terminal = outcome.terminal.load(std::memory_order_acquire);
        const int action_count = stop_actions.load(std::memory_order_acquire);
        if (outcome.completions.load(std::memory_order_acquire) != 1
            || (terminal != ETerminal::Value
                && terminal != ETerminal::Stopped)
            || action_count < 0 || action_count > 1
            || (terminal == ETerminal::Stopped && action_count != 1)
            || (terminal == ETerminal::Value && action_count != 0))
            ++bad_races;
    }
    check(bad_races == 0,
          "value/stop races have one terminal and one stop winner");

    std::printf(failures == 0 ? "\nALL CHECKS PASSED\n"
                              : "\n%d CHECK(S) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}

// AsyncScope structured-lifetime acceptance test (CPU only, no renderer).

#include <lux/engine/runtime/execution/AsyncCallbackSender.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScope.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>
#include <lux/engine/runtime/execution/testing/AsyncCloseTestDriver.hpp>

#include <stdexec/execution.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

namespace ex = stdexec;

int main()
{
    int failures = 0;
    auto check = [&](bool condition, const char* message)
    {
        if (condition) std::printf("[ ok ] %s\n", message);
        else           { std::printf("[FAIL] %s\n", message); ++failures; }
    };

    lux::exec::AsyncRuntimeBuilder builder;
    auto plan = std::move(builder).compile();
    if (!plan)
        return 1;
    lux::exec::AsyncRuntime runtime(
        std::move(*plan),
        lux::exec::AsyncRuntimeConfig{
            .blocking_io_threads = 1,
            .background_cpu_concurrency = 2});

    std::atomic<bool> main_ran{false};
    lux::exec::AsyncScope scope(runtime);

    const bool accepted = lux::exec::spawn(scope,
        ex::schedule(lux::exec::mainThreadScheduler(runtime))
      | ex::then([&]() noexcept
        { main_ran.store(true, std::memory_order_release); })
      | ex::upon_stopped([]() noexcept {}));

    check(accepted, "open owner scope accepts a child sender");
    lux::exec::testing::closeScope(scope, runtime);
    check(main_ran.load(std::memory_order_acquire),
          "close drives and waits for the main continuation");
    check(!scope.isOpen(), "close permanently seals the scope");

    std::atomic<bool> rejected_body_ran{false};
    const bool rejected = lux::exec::spawn(scope,
        ex::just()
      | ex::then([&]() noexcept
        { rejected_body_ran.store(true, std::memory_order_release); }));
    check(!rejected, "closed owner scope rejects a new child sender");
    check(!rejected_body_ran.load(std::memory_order_acquire),
          "a rejected sender is never started");

    std::atomic<bool> timer_stopped{false};
    const auto timer_close_started = std::chrono::steady_clock::now();
    {
        lux::exec::AsyncScope timer_scope(runtime);
        check(lux::exec::spawn(
                  timer_scope,
                  lux::exec::scheduleAfter(runtime, std::chrono::hours{1})
                      | ex::upon_stopped(
                          [&]() noexcept
                          {
                              timer_stopped.store(
                                  true,
                                  std::memory_order_release);
                          })),
              "scope accepts an Asio timer sender");
        lux::exec::testing::closeScope(timer_scope, runtime);
    }
    check(timer_stopped.load(std::memory_order_acquire),
          "scope stop cancels its Asio timer exactly once");
    check(std::chrono::steady_clock::now() - timer_close_started <
              std::chrono::seconds{1},
          "timer cancellation does not wait for the deadline");

    // Destructor is a passive stop backstop, not a hidden blocking boundary.
    std::atomic<bool> destructor_joined{false};
    {
        lux::exec::AsyncScope automatic(runtime);
        check(lux::exec::spawn(automatic,
            ex::schedule(lux::exec::mainThreadScheduler(runtime))
          | ex::then([&]() noexcept
            { destructor_joined.store(true, std::memory_order_release); })
          | ex::upon_stopped([&]() noexcept
            { destructor_joined.store(true, std::memory_order_release); })),
          "automatic scope accepts work");
    }
    for (int turn = 0;
         turn < 1024 &&
             !destructor_joined.load(std::memory_order_acquire);
         ++turn)
        (void)runtime.drainMainThreadCompletions();
    check(destructor_joined.load(std::memory_order_acquire),
          "AsyncScope destructor requests stop without losing the terminal");

    // A stoppable external source must not make close depend on a callback
    // that may never arrive. Its stop action retires the observer first, then
    // callbackSender completes stopped and the scope becomes empty.
    std::atomic<bool> external_joined{false};
    std::atomic<bool> external_stopped{false};
    int stop_actions = 0;
    lux::cxx::move_only_function<void()> complete_external;
    {
        lux::exec::AsyncScope external(runtime);
        const bool external_started = lux::exec::spawn(external,
            lux::exec::callbackSender<int>(
                [&complete_external, &stop_actions](auto completer)
                    mutable noexcept
                    -> lux::exec::AsyncStopAction
                {
                    complete_external =
                        [c = std::move(completer)]() mutable noexcept
                        { std::move(c).complete(42); };
                    return lux::exec::AsyncStopAction{
                        [&complete_external, &stop_actions]() noexcept
                        {
                            ++stop_actions;
                            complete_external.reset();
                        }
                    };
                })
          | ex::then([&](int value) noexcept
            { external_joined.store(value == 42, std::memory_order_release); })
          | ex::upon_error([&](lux::exec::AsyncCallbackError) noexcept
            { external_joined.store(false, std::memory_order_release); })
          | ex::upon_stopped([&]() noexcept
            { external_stopped.store(true, std::memory_order_release); }));
        check(external_started, "scope accepts an externally completed sender");
        lux::exec::testing::closeScope(external, runtime);
    }
    check(stop_actions == 1,
          "close invokes the external stop action exactly once");
    check(external_stopped.load(std::memory_order_acquire),
          "close observes the external sender's stopped terminal");
    check(!external_joined.load(std::memory_order_acquire),
          "cancelled external work does not publish a value");

    // Exercise operation-state destruction through a real async_scope while
    // completion on a worker races the owner-thread stop request. The late
    // callback deliberately remains alive as the modeled reaper handoff.
    constexpr int kScopeRaceRounds = 250;
    int bad_scope_races = 0;
    for (int round = 0; round < kScopeRaceRounds; ++round)
    {
        lux::exec::AsyncScope racing(runtime);
        std::atomic<int> terminals{0};
        std::atomic<int> stop_actions_race{0};
        std::atomic<bool> go{false};
        auto complete =
            std::make_shared<lux::cxx::move_only_function<void()>>();

        const bool race_started = lux::exec::spawn(racing,
            lux::exec::callbackSender<int>(
                [complete, &stop_actions_race](auto completer)
                    mutable noexcept -> lux::exec::AsyncStopAction
                {
                    *complete =
                        [c = std::move(completer)]() mutable noexcept
                        { std::move(c).complete(1); };
                    return lux::exec::AsyncStopAction{
                        [&stop_actions_race]() noexcept
                        {
                            stop_actions_race.fetch_add(
                                1,
                                std::memory_order_relaxed
                            );
                        }
                    };
                })
          | ex::then([&](int) noexcept
            { terminals.fetch_add(1, std::memory_order_acq_rel); })
          | ex::upon_error([&](lux::exec::AsyncCallbackError) noexcept
            { terminals.fetch_add(100, std::memory_order_acq_rel); })
          | ex::upon_stopped([&]() noexcept
            { terminals.fetch_add(1, std::memory_order_acq_rel); }));
        if (!race_started)
        {
            ++bad_scope_races;
            continue;
        }

        std::thread completing([&]() noexcept
        {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            (*complete)();
        });
        go.store(true, std::memory_order_release);
        racing.requestStop();
        completing.join();
        lux::exec::testing::closeScope(racing, runtime);

        const int actions =
            stop_actions_race.load(std::memory_order_acquire);
        if (terminals.load(std::memory_order_acquire) != 1
            || actions < 0 || actions > 1)
            ++bad_scope_races;
    }
    check(bad_scope_races == 0,
          "AsyncScope completion/stop races join and destroy safely");

    (void)lux::exec::testing::closeRuntime(runtime);
    std::printf(failures == 0 ? "\nALL CHECKS PASSED\n"
                              : "\n%d CHECK(S) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/process/TaskScope.hpp>

#include <exec/materialize.hpp>
#include <stdexec/execution.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <concepts>
#include <thread>
#include <type_traits>
#include <utility>

static_assert(lux::process::detail::TaskScopeLifetimeSender<decltype(stdexec::just())>);
static_assert(!lux::process::detail::TaskScopeLifetimeSender<decltype(stdexec::just(42))>);
static_assert(!lux::process::detail::TaskScopeLifetimeSender<decltype(stdexec::just_error(42))>);

namespace
{
    [[nodiscard]] lux::process::ExecutionRuntime makeRuntime(std::size_t concurrency = 2U)
    {
        auto created = lux::process::ExecutionRuntime::create({concurrency, 8U, 8U, {8U}});
        assert(created);
        return std::move(*created);
    }

    void closeScope(lux::process::TaskScope& scope)
    {
        const auto result = stdexec::sync_wait(scope.close());
        assert(result);
        assert(scope.closed());
    }

    template<stdexec::sender Sender>
    [[nodiscard]] auto consumeTerminal(Sender&& sender)
    {
        return exec::materialize(std::forward<Sender>(sender)) |
            stdexec::then([]<class Tag, class... Values>(Tag, Values&&...) noexcept {});
    }

    void testOwnsOperationUntilTerminal()
    {
        auto runtime = makeRuntime(1U);
        lux::process::TaskScope scope;
        std::atomic_bool entered{};
        std::atomic_bool release{};
        std::atomic_bool completed{};
        auto sender = stdexec::schedule(runtime.cpu()) |
            stdexec::then([&]() noexcept {
                entered.store(true, std::memory_order_release);
                entered.notify_all();
                release.wait(false, std::memory_order_acquire);
                completed.store(true, std::memory_order_release);
            });
        assert(scope.start(consumeTerminal(std::move(sender))));
        entered.wait(false, std::memory_order_acquire);

        std::atomic_bool close_returned{};
        std::jthread closer([&] {
            closeScope(scope);
            close_returned.store(true, std::memory_order_release);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        assert(!close_returned.load(std::memory_order_acquire));
        release.store(true, std::memory_order_release);
        release.notify_all();
        closer.join();
        assert(completed.load(std::memory_order_acquire));

        const auto rejected = scope.start(consumeTerminal(stdexec::schedule(runtime.cpu())));
        assert(!rejected);
        assert(rejected.error() == lux::process::ETaskStartError::STOPPING);
        runtime.requestStop();
        assert(runtime.join());
    }

    void testStopPropagationAndCloseRace()
    {
        auto runtime = makeRuntime(1U);
        lux::process::TaskScope scope;
        std::atomic_bool first_entered{};
        std::atomic_bool first_release{};
        auto first = stdexec::schedule(runtime.cpu()) |
            stdexec::then([&]() noexcept {
                first_entered.store(true, std::memory_order_release);
                first_entered.notify_all();
                first_release.wait(false, std::memory_order_acquire);
            });
        assert(scope.start(consumeTerminal(std::move(first))));
        first_entered.wait(false, std::memory_order_acquire);

        std::atomic_bool should_not_run{};
        auto queued = stdexec::schedule(runtime.cpu()) |
            stdexec::then([&]() noexcept { should_not_run.store(true, std::memory_order_release); });
        assert(scope.start(consumeTerminal(std::move(queued))));
        scope.requestStop();
        first_release.store(true, std::memory_order_release);
        first_release.notify_all();
        closeScope(scope);
        assert(!should_not_run.load(std::memory_order_acquire));

        runtime.requestStop();
        assert(runtime.join());
    }

    void testMainCompletionRequiresDrain()
    {
        auto runtime = makeRuntime();
        lux::process::TaskScope scope;
        std::atomic_bool ran{};
        auto sender = stdexec::schedule(runtime.main()) |
            stdexec::then([&]() noexcept { ran.store(true, std::memory_order_release); });
        assert(scope.start(consumeTerminal(std::move(sender))));

        std::atomic_bool close_returned{};
        std::jthread closer([&] {
            closeScope(scope);
            close_returned.store(true, std::memory_order_release);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        assert(!close_returned.load(std::memory_order_acquire));
        const auto drained = runtime.drainMain();
        assert(drained && *drained == 1U);
        closer.join();
        assert(!ran.load(std::memory_order_acquire));

        runtime.requestStop();
        assert(runtime.join());
    }

    void testInlineReentrantStop()
    {
        lux::process::TaskScope scope;
        const auto started = scope.start(
            stdexec::just() |
            stdexec::then([&scope]() noexcept { scope.requestStop(); })
        );
        assert(started);
        assert(scope.stopping());
        closeScope(scope);
    }

    void testStartCloseAdmissionRace()
    {
        lux::process::TaskScope scope;
        std::atomic_bool entered{};
        std::atomic_bool release{};
        std::atomic_bool start_returned{};
        std::jthread starter([&] {
            const auto started = scope.start(
                stdexec::just() |
                stdexec::then([&]() noexcept {
                    entered.store(true, std::memory_order_release);
                    entered.notify_all();
                    release.wait(false, std::memory_order_acquire);
                })
            );
            assert(started);
            start_returned.store(true, std::memory_order_release);
        });
        entered.wait(false, std::memory_order_acquire);

        std::atomic_bool close_returned{};
        std::jthread closer([&] {
            closeScope(scope);
            close_returned.store(true, std::memory_order_release);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        assert(!start_returned.load(std::memory_order_acquire));
        assert(!close_returned.load(std::memory_order_acquire));

        release.store(true, std::memory_order_release);
        release.notify_all();
        starter.join();
        closer.join();
        assert(start_returned.load(std::memory_order_acquire));
        assert(close_returned.load(std::memory_order_acquire));
    }

    void testStopStartRace()
    {
        for (int iteration = 0; iteration != 100; ++iteration)
        {
            lux::process::TaskScope scope;
            std::atomic<int> ready{};
            std::atomic_bool go{};
            std::atomic_bool ran{};
            bool admitted{};
            lux::process::ETaskStartError rejection{};
            std::jthread starter([&] {
                ready.fetch_add(1, std::memory_order_release);
                ready.notify_all();
                go.wait(false, std::memory_order_acquire);
                const auto started = scope.start(
                    stdexec::just() |
                    stdexec::then([&]() noexcept { ran.store(true, std::memory_order_release); })
                );
                admitted = static_cast<bool>(started);
                if (!started)
                    rejection = started.error();
            });
            std::jthread stopper([&] {
                ready.fetch_add(1, std::memory_order_release);
                ready.notify_all();
                go.wait(false, std::memory_order_acquire);
                scope.requestStop();
            });
            auto observed = ready.load(std::memory_order_acquire);
            while (observed != 2)
            {
                ready.wait(observed, std::memory_order_acquire);
                observed = ready.load(std::memory_order_acquire);
            }
            go.store(true, std::memory_order_release);
            go.notify_all();
            starter.join();
            stopper.join();
            closeScope(scope);
            if (admitted)
                assert(ran.load(std::memory_order_acquire));
            else
                assert(rejection == lux::process::ETaskStartError::STOPPING);
        }
    }
}

int main()
{
    testOwnsOperationUntilTerminal();
    testStopPropagationAndCloseRace();
    testMainCompletionRequiresDrain();
    testInlineReentrantStop();
    testStartCloseAdmissionRace();
    testStopStartRace();
    return 0;
}

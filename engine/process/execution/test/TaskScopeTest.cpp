#include <lux/engine/process/TaskScope.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <semaphore>
#include <thread>

namespace
{
    using namespace lux::process;
    using Value = lux::cxx::expected<int, EExecutionError>;

    void destroyedScope(bool shutdown)
    {
        std::binary_semaphore entered{0}, release{0}, destroyed{0};
        std::atomic<bool> waited{}, stopped{};
        unsigned delivered{};
        std::weak_ptr<int> operation_owner;
        const auto owner_thread = std::this_thread::get_id();
        {
            auto runtime = ExecutionRuntime::create({1, 16, 16, {16}});
            assert(runtime);
            auto scope = std::make_unique<TaskScope>(*runtime);
            auto pin = std::make_shared<int>(42);
            operation_owner = pin;
            const auto task = scope->submit(
                {"Scope outlived by accepted task", "test"},
                [&](TaskReporter reporter) noexcept
                {
                    return stdexec::then(
                        stdexec::schedule(runtime->cpu()),
                        [&, reporter, pin]() noexcept -> Value
                        {
                            entered.release();
                            release.acquire();
                            stopped.store(reporter.stopToken().stop_requested());
                            reporter.setPhase("scope already destroyed");
                            reporter.setProgress(1, 1);
                            return *pin;
                        }
                    );
                },
                [&, pin](TTaskResult<int, EExecutionError>&& value) noexcept
                {
                    assert(std::this_thread::get_id() == owner_thread);
                    assert(value && *value == *pin);
                    ++delivered;
                }
            );
            assert(task);
            pin.reset();
            entered.acquire();
            // Only the watchdog releases a blocking old destructor. No deadlock or false pass.
            std::thread watchdog(
                [&]
                {
                    if (!destroyed.try_acquire_for(std::chrono::seconds(2)))
                    {
                        waited.store(true);
                        release.release();
                    }
                }
            );
            scope.reset();
            destroyed.release();
            watchdog.join();
            if (waited.load())
            {
                std::fputs("FAIL T01: TaskScope destructor waited for the blocked worker\n", stderr);
                std::fflush(stderr);
                std::abort();
            }
            assert(delivered == 0 && !operation_owner.expired());
            const auto info = runtime->taskInfo(*task);
            assert(info && info->cancel_requested && !info->finished);
            // Reuse allocator storage while the accepted operation still owns its group.
            for (unsigned index{}; index != 128; ++index)
            {
                TaskScope replacement{*runtime};
                assert(replacement.settled());
            }
            release.release();
            if (!shutdown)
            {
                assert(runtime->waitUntil([&]() noexcept { return delivered == 1; }));
                assert(operation_owner.expired());
                const auto completed = runtime->taskInfo(*task);
                assert(completed && completed->state == ETaskState::SUCCEEDED);
                assert(runtime->collectCompletions() && delivered == 1);
            }
            // The other path exercises the original Runtime shutdown/drain, with no surviving scope.
        }
        assert(stopped.load() && delivered == 1 && operation_owner.expired());
        std::printf(
            "PASS %s: owner delivery once; cancellation observed; accepted operation released\n",
            shutdown ? "T04 runtime shutdown" : "T01/T02/T03 destroyed scope"
        );
    }

    void explicitJoin()
    {
        auto runtime = ExecutionRuntime::create({1, 16, 16, {16}});
        assert(runtime);
        TaskScope scope{*runtime};
        std::binary_semaphore entered{0}, release{0};
        bool delivered{};
        assert(scope.submit(
            {"Explicit join", "test"},
            [&](TaskReporter) noexcept
            {
                return stdexec::then(
                    stdexec::schedule(runtime->cpu()),
                    [&]() noexcept -> Value
                    {
                        entered.release();
                        release.acquire();
                        return 7;
                    }
                );
            },
            [&](TTaskResult<int, EExecutionError>&& value) noexcept
            {
                assert(value && *value == 7);
                delivered = true;
                assert(!scope.settled()); // Accounting ends only after synchronous delivery returns.
            }
        ));
        entered.acquire();
        assert(!scope.settled() && !delivered);
        release.release();
        assert(scope.join() && scope.settled() && delivered);
        const auto rejected = scope.submit({}, [](TaskReporter) noexcept { return stdexec::just(); });
        assert(!rejected && rejected.error() == EExecutionError::STOPPING);
        std::puts("PASS T05 explicit join waits for delivery and closes admission");
    }
} // namespace

int main()
{
    destroyedScope(false);
    destroyedScope(true);
    explicitJoin();
}

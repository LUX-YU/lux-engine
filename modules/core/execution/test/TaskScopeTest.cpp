#include <lux/engine/core/execution/TaskScope.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <semaphore>
#include <thread>

namespace
{
    using namespace lux::execution;
    using Value = lux::cxx::expected<int, EExecutionError>;

    void unmonitoredExecution()
    {
        auto runtime = ExecutionRuntime::create({1, 1, {16}});
        assert(runtime);
        TaskScope monitored{*runtime};
        std::binary_semaphore entered{0}, release{0};
        assert(monitored.submit(
            {"Monitored capacity", "test"},
            [&](TaskReporter) noexcept
            {
                return stdexec::schedule(runtime->cpu()) | stdexec::then(
                                                               [&]() noexcept
                                                               {
                                                                   entered.release();
                                                                   release.acquire();
                                                               }
                                                           );
            }
        ));
        entered.acquire();
        unsigned completed = 0;
        assert(runtime->spawn(stdexec::just() | stdexec::then([&]() noexcept { ++completed; })));
        assert(completed == 1 && runtime->taskInfos().size() == 1);
        assert(
            runtime->spawn(stdexec::schedule(runtime->mainScheduler()) | stdexec::then([&]() noexcept { ++completed; }))
        );
        assert(!runtime->settled());
        runtime->requestStop();
        assert(!runtime->spawn(stdexec::just()));
        assert(runtime->dispatchMainCompletions() && completed == 2);
        release.release();
        assert(runtime->waitUntil([&]() noexcept { return runtime->settled(); }));
        assert(runtime->join());
    }

    void destroyedScope(bool shutdown)
    {
        std::binary_semaphore entered{0}, release{0}, destroyed{0};
        std::atomic<bool> waited{}, stopped{};
        unsigned delivered{};
        std::weak_ptr<int> operation_owner;
        const auto owner_thread = std::this_thread::get_id();
        {
            auto runtime = ExecutionRuntime::create({1, 16, {16}});
            assert(runtime);
            auto scope = std::make_unique<TaskScope>(*runtime);
            auto pin = std::make_shared<int>(42);
            operation_owner = pin;
            const auto task = scope->submit(
                {"Scope outlived by accepted task", "test"},
                [&,
                 make_work =
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
                 consume_result =
                     [&, pin](TTaskResult<int, EExecutionError>&& value) noexcept
                 {
                     assert(std::this_thread::get_id() == owner_thread);
                     assert(value && *value == *pin);
                     ++delivered;
                 }](lux::execution::TaskReporter reporter) mutable noexcept
                {
                    auto ready = stdexec::continues_on(
                        lux::execution::taskResult(make_work(reporter)),
                        runtime->mainScheduler()
                    );
                    return stdexec::then(
                        std::move(ready),
                        [consume_result = std::move(consume_result)](auto result) mutable noexcept
                        {
                            const auto outcome = lux::execution::taskState(result);
                            consume_result(std::move(result));
                            return outcome;
                        }
                    );
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
                assert(runtime->dispatchMainCompletions() && delivered == 1);
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
        auto runtime = ExecutionRuntime::create({1, 16, {16}});
        assert(runtime);
        TaskScope scope{*runtime};
        std::binary_semaphore entered{0}, release{0};
        bool delivered{};
        assert(scope.submit(
            {"Explicit join", "test"},
            [&,
             make_work =
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
             consume_result =
                 [&](TTaskResult<int, EExecutionError>&& value) noexcept
             {
                 assert(value && *value == 7);
                 delivered = true;
                 assert(!scope.settled()); // Accounting ends only after synchronous delivery returns.
             }](lux::execution::TaskReporter reporter) mutable noexcept
            {
                auto ready =
                    stdexec::continues_on(lux::execution::taskResult(make_work(reporter)), runtime->mainScheduler());
                return stdexec::then(
                    std::move(ready),
                    [consume_result = std::move(consume_result)](auto result) mutable noexcept
                    {
                        const auto outcome = lux::execution::taskState(result);
                        consume_result(std::move(result));
                        return outcome;
                    }
                );
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

    struct CallbackCode final
    {
        std::atomic_bool* executing;

        explicit CallbackCode(std::atomic_bool& active) noexcept : executing(&active) {}

        ~CallbackCode() noexcept
        {
            assert(!executing->load());
        }
    };

    struct DestroyingReceiver final
    {
        using receiver_concept = stdexec::receiver_t;
        void** operation;
        void (*destroy)(void*) noexcept;
        std::shared_ptr<const void> code;
        std::atomic_bool* executing;
        std::atomic_bool* completed;
        std::binary_semaphore* entered{};
        std::binary_semaphore* resume{};

        auto get_env() const noexcept
        {
            return stdexec::env{stdexec::prop{detail::getCompletionCode, code}};
        }

        void set_value() && noexcept
        {
            auto* active = executing;
            auto* done = completed;
            auto* entered_callback = entered;
            auto* resume_callback = resume;
            std::weak_ptr<const void> lifetime = code;
            auto* state = std::exchange(*operation, nullptr);
            const auto release = destroy;
            active->store(true);
            release(state);              // The receiver and its only owned code pin cease to exist here.
            assert(!lifetime.expired()); // The publishing Core frame retains the callback code through return.
            if (entered_callback)
            {
                entered_callback->release();
                resume_callback->acquire();
            }
            active->store(false);
            done->store(true, std::memory_order_release);
        }
    };

    void groupCompletionCodeLifetime(bool asynchronous)
    {
        auto runtime = ExecutionRuntime::create({1, 8, {16}});
        assert(runtime);
        TaskScope group{*runtime};
        std::binary_semaphore entered{0}, release{0}, callback_entered{0}, resume_callback{0};
        if (asynchronous)
        {
            assert(group.submit(
                {},
                [&](TaskReporter) noexcept
                {
                    return stdexec::schedule(runtime->cpu()) | stdexec::then(
                                                                   [&]() noexcept
                                                                   {
                                                                       entered.release();
                                                                       release.acquire();
                                                                   }
                                                               );
                }
            ));
            entered.acquire();
        }
        std::atomic_bool executing{}, completed{};
        auto code = std::make_shared<CallbackCode>(executing);
        std::weak_ptr<const void> lifetime = code;
        using Operation = stdexec::connect_result_t<TaskScope::CompletionSender, DestroyingReceiver>;
        void* state = nullptr;
        state = new Operation(stdexec::connect(
            group.whenSettled(),
            DestroyingReceiver{
                &state,
                [](void* value) noexcept { delete static_cast<Operation*>(value); },
                code,
                &executing,
                &completed,
                asynchronous ? &callback_entered : nullptr,
                asynchronous ? &resume_callback : nullptr
            }
        ));
        code.reset();
        stdexec::start(*static_cast<Operation*>(state));
        if (asynchronous)
        {
            assert(!completed.load());
            release.release();
            callback_entered.acquire();
            // Account removal alone is not final delivery. Consume every earlier epoch while the
            // Receiver is held, then require a wake after its completion instead of waiting forever.
            const auto epoch = runtime->wakeEpoch();
            resume_callback.release();
            runtime->waitForWork(epoch, std::chrono::steady_clock::now() + std::chrono::seconds(1));
            assert(runtime->wakeEpoch() != epoch && completed.load(std::memory_order_acquire));
        }
        assert(runtime->waitUntil([&]() noexcept { return completed.load(std::memory_order_acquire); }));
        runtime->requestStop();
        assert(runtime->waitUntil([&]() noexcept { return runtime->settled(); }));
        assert(runtime->join());
        assert(lifetime.expired() && state == nullptr);
    }

    void asynchronousGroupJoin()
    {
        auto runtime = ExecutionRuntime::create({1, 8, {16}});
        assert(runtime);
        TaskScope group{*runtime};
        std::binary_semaphore entered{0}, release{0};
        std::atomic_bool consumed{};
        unsigned settled{};
        assert(group.submit(
            {},
            [&](TaskReporter) noexcept
            {
                return stdexec::schedule(runtime->cpu()) | stdexec::then(
                                                               [&]() noexcept
                                                               {
                                                                   entered.release();
                                                                   release.acquire();
                                                                   consumed.store(true, std::memory_order_release);
                                                               }
                                                           );
            }
        ));
        entered.acquire();
        for (unsigned i = 0; i != 2; ++i)
        {
            assert(runtime->spawn(stdexec::then(
                stdexec::continues_on(group.whenSettled(), runtime->mainScheduler()),
                [&]() noexcept
                {
                    assert(consumed.load(std::memory_order_acquire) && group.settled());
                    ++settled;
                }
            )));
        }
        assert(!group.submit({}, [](TaskReporter) noexcept { return stdexec::just(); }));
        assert(settled == 0);
        release.release();
        assert(runtime->waitUntil([&]() noexcept { return settled == 2; }));
        // The already-settled path completes synchronously; its receiver may destroy the spawned state.
        assert(runtime->spawn(stdexec::then(group.whenSettled(), [&]() noexcept { ++settled; })));
        assert(settled == 3);
        runtime->requestStop();
        assert(runtime->waitUntil([&]() noexcept { return runtime->settled(); }));
        assert(runtime->join());
    }

    void capacityWaitDoesNotCloseAdmission()
    {
        auto runtime = ExecutionRuntime::create({1, 1, {16}});
        assert(runtime);
        TaskScope tasks{*runtime};
        TaskScope waiting{*runtime};
        std::binary_semaphore entered{0}, release{0};
        assert(tasks.submit(
            {"Occupy task capacity", "test"},
            [&](TaskReporter) noexcept
            {
                return stdexec::schedule(runtime->cpu()) | stdexec::then(
                                                               [&]() noexcept
                                                               {
                                                                   entered.release();
                                                                   release.acquire();
                                                               }
                                                           );
            }
        ));
        entered.acquire();
        std::atomic<unsigned> ready{}, stopped{};
        auto wait = [&]() noexcept
        {
            return waiting.whenCapacityAvailable() | stdexec::then([&]() noexcept { ready.fetch_add(1); }) |
                   stdexec::upon_stopped([&]() noexcept { stopped.fetch_add(1); });
        };
        assert(runtime->spawn(wait()));
        assert(ready.load() == 0 && stopped.load() == 0);
        release.release();
        assert(runtime->waitUntil([&]() noexcept { return ready.load() == 1; }));
        assert(waiting.submit({"Admission remains open", "test"}, [](TaskReporter) noexcept { return stdexec::just(); })
        );
        // Available capacity completes in start(), including destruction of the spawned operation.
        assert(runtime->spawn(wait()));
        assert(ready.load() == 2 && stopped.load() == 0);
        waiting.requestStop();
        assert(runtime->spawn(wait()));
        assert(ready.load() == 2 && stopped.load() == 1);
    }

    void capacityWaitStops(bool stop_runtime)
    {
        auto runtime = ExecutionRuntime::create({1, 1, {16}});
        assert(runtime);
        TaskScope tasks{*runtime};
        auto waiting = std::make_unique<TaskScope>(*runtime);
        std::binary_semaphore entered{0}, release{0};
        assert(tasks.submit(
            {"Capacity remains occupied", "test"},
            [&](TaskReporter) noexcept
            {
                return stdexec::schedule(runtime->cpu()) | stdexec::then(
                                                               [&]() noexcept
                                                               {
                                                                   entered.release();
                                                                   release.acquire();
                                                               }
                                                           );
            }
        ));
        entered.acquire();
        unsigned ready{}, stopped{};
        assert(runtime->spawn(
            waiting->whenCapacityAvailable() | stdexec::then([&]() noexcept { ++ready; }) |
            stdexec::upon_stopped([&]() noexcept { ++stopped; })
        ));
        if (stop_runtime)
        {
            runtime->requestStop();
        }
        else
        {
            waiting.reset();
        }
        assert(ready == 0 && stopped == 1);
        release.release();
        assert(runtime->waitUntil([&]() noexcept { return tasks.settled(); }));
        assert(ready == 0 && stopped == 1);
    }

    void terminalReleasesCapacity()
    {
        auto runtime = ExecutionRuntime::create({1, 1, {16}, {}, 2});
        assert(runtime);
        TaskScope tasks{*runtime};
        std::atomic<unsigned> completed{};
        for (unsigned i = 0; i != 128; ++i)
        {
            auto accepted = tasks.submit(
                {"Finite CPU work", "test"},
                [&](TaskReporter) noexcept
                {
                    return stdexec::schedule(runtime->cpu()) |
                           stdexec::then([&]() noexcept { completed.fetch_add(1, std::memory_order_release); });
                }
            );
            assert(accepted);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!tasks.settled())
            {
                assert(std::chrono::steady_clock::now() < deadline);
                std::this_thread::yield();
            }
            assert(completed.load(std::memory_order_acquire) == i + 1);
        }
        // No Main dispatch above: finished IO/CPU work must return its own admission capacity.
        assert(runtime->taskInfos().size() == 2);
        assert(tasks.submit({"Sync completion", "test"}, [](TaskReporter) noexcept { return stdexec::just(); }));
        assert(tasks.submit(
            {"Nested allocation", "test"},
            [](TaskReporter reporter) noexcept
            {
                auto allocator = stdexec::read_env(stdexec::get_allocator);
                return stdexec::then(
                    std::move(allocator),
                    [reporter](auto allocator) noexcept
                    {
                        auto* memory = allocator.allocate(16);
                        allocator.deallocate(memory, 16);
                        // A nested allocation cannot release the outer task's metadata.
                        reporter.setProgress(1, 1);
                    }
                );
            }
        ));
        assert(tasks.settled());
        assert(tasks.submit(
            {"Domain failure", "test"},
            [](TaskReporter) noexcept
            {
                return stdexec::just(
                    lux::cxx::expected<void, EExecutionError>{lux::cxx::unexpected(EExecutionError::BACKEND_FAILURE)}
                );
            }
        ));
        assert(
            std::ranges::any_of(runtime->taskInfos(), [](const auto& info) { return info.state == ETaskState::FAILED; })
        );
        assert(tasks.submit({"Stopped", "test"}, [](TaskReporter) noexcept { return stdexec::just_stopped(); }));
        const auto infos = runtime->taskInfos();
        assert(std::ranges::any_of(infos, [](const auto& info) { return info.state == ETaskState::CANCELLED; }));
        std::puts("PASS terminal capacity: no Main hop, bounded history, synchronous/error/stopped completion");
    }

} // namespace

int main()
{
    unmonitoredExecution();
    destroyedScope(false);
    destroyedScope(true);
    explicitJoin();
    asynchronousGroupJoin();
    groupCompletionCodeLifetime(false);
    groupCompletionCodeLifetime(true);
    terminalReleasesCapacity();
    capacityWaitDoesNotCloseAdmission();
    capacityWaitStops(false);
    capacityWaitStops(true);
}

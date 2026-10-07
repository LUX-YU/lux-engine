#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/process/ObjectScheduler.hpp>
#include <memory>
#include <random>
#include <stop_token>
#include <thread>

using namespace lux;
namespace
{
    struct Result final
    {
        unsigned value{}, stopped{}, error{};
        process::EExecutionError failure{};
        std::thread::id thread;
        unsigned count() const noexcept
        {
            return value + stopped + error;
        }
    };
    struct StopEnv final
    {
        std::stop_token token;
        std::stop_token query(stdexec::get_stop_token_t) const noexcept
        {
            return token;
        }
    };
    struct Receiver final
    {
        using receiver_concept = stdexec::receiver_t;
        Result* result{};
        std::stop_token token;
        StopEnv get_env() const noexcept
        {
            return {token};
        }
        void set_value() && noexcept
        {
            assert(result->count() == 0);
            ++result->value;
            result->thread = std::this_thread::get_id();
        }
        void set_stopped() && noexcept
        {
            assert(result->count() == 0);
            ++result->stopped;
        }
        void set_error(process::EExecutionError error) && noexcept
        {
            assert(result->count() == 0);
            ++result->error;
            result->failure = error;
        }
        void set_error(std::exception_ptr) && noexcept
        {
            std::abort();
        }
    };
    class Target final : public object::LuxObject
    {
    public:
        void close() noexcept
        {
            beginDestruction();
        }
    };

    void lifetime()
    {
        auto& runtime = object::ObjectRuntime::instance();
        for (unsigned mode{}; mode < 3; ++mode)
        {
            auto target = std::make_unique<Target>();
            const auto scheduler = process::objectScheduler(*target);
            assert(scheduler == process::objectScheduler(*target));
            Result result;
            unsigned calls{};
            auto sender =
                stdexec::just() | stdexec::continues_on(scheduler) | stdexec::then([&]() noexcept { ++calls; });
            auto op = stdexec::connect(std::move(sender), Receiver{&result});
            stdexec::start(op);
            assert(result.count() == 0);
            if (mode == 1)
            {
                target.reset();
            }
            if (mode == 2)
            {
                target->close();
            }
            assert(runtime.dispatchPending() == 1);
            assert(result.count() == 1 && calls == (mode == 0));
            assert(result.stopped == (mode != 0));
            if (mode == 0)
            {
                assert(result.thread == std::this_thread::get_id());
            }
        }
        Result closed;
        auto op = stdexec::connect(stdexec::schedule(process::ObjectScheduler{}), Receiver{&closed});
        stdexec::start(op);
        assert(closed.stopped == 1);
    }

    void cancellationAndCapacity()
    {
        auto& runtime = object::ObjectRuntime::instance();
        Target target;
        auto scheduler = process::objectScheduler(target);
        for (bool before : {true, false})
        {
            std::stop_source stop;
            Result result;
            auto op = stdexec::connect(stdexec::schedule(scheduler), Receiver{&result, stop.get_token()});
            const auto stats = runtime.statistics();
            if (before)
            {
                stop.request_stop();
            }
            stdexec::start(op);
            if (!before)
            {
                std::thread worker([&] { stop.request_stop(); });
                worker.join();
                assert(result.count() == 0 && runtime.statistics().pending == stats.pending + 1);
                assert(runtime.dispatchPending() == 1);
            }
            else
            {
                assert(runtime.statistics().posted == stats.posted);
            }
            assert(result.stopped == 1);
        }
        const auto capacity = runtime.statistics().capacity_per_batch;
        unsigned received{};
        for (std::size_t i{}; i < capacity; ++i)
        {
            assert(
                object::post(
                    target.target(),
                    [&](object::LuxObject* value) noexcept
                    {
                        assert(value == &target);
                        ++received;
                    }
                ) == object::EObjectPostStatus::POSTED
            );
        }
        Result full;
        auto op = stdexec::connect(stdexec::schedule(scheduler), Receiver{&full});
        stdexec::start(op);
        assert(full.error == 1 && full.failure == process::EExecutionError::CAPACITY_EXCEEDED);
        assert(runtime.dispatchPending() == capacity && received == capacity);
    }

    void threadAndBorrow()
    {
        auto& runtime = object::ObjectRuntime::instance();
        Target target;
        object::LuxObject child;
        Result result;
        unsigned calls{};
        auto scheduler = process::objectScheduler(target);
        // Only this immediate continuation borrows target; worker owns only scheduler/operation state.
        auto sender = stdexec::just() | stdexec::continues_on(scheduler) |
                      stdexec::then(
                          [&]() noexcept
                          {
                              assert(runtime.isCurrent());
                              auto busy = target.addChild(child);
                              assert(!busy && busy.error() == object::EObjectTreeError::BUSY);
                              ++calls;
                          }
                      );
        auto op = stdexec::connect(std::move(sender), Receiver{&result});
        std::thread worker([&] { stdexec::start(op); });
        worker.join();
        assert(calls == 0);
        assert(runtime.dispatchPending() == 1);
        assert(calls == 1 && result.value == 1 && result.thread == std::this_thread::get_id());
    }

    void completionDestroysOperation()
    {
        Target target;
        Result result;
        struct DeletingReceiver final
        {
            using receiver_concept = stdexec::receiver_t;
            Result* result;
            void** owner;
            void (*destroy)(void*) noexcept;
            stdexec::empty_env get_env() const noexcept
            {
                return {};
            }
            void set_value() && noexcept
            {
                ++result->value;
                auto* value = std::exchange(*owner, nullptr);
                destroy(value);
            }
            void set_error(process::EExecutionError) && noexcept
            {
                std::abort();
            }
            void set_stopped() && noexcept
            {
                std::abort();
            }
        };
        using Operation = decltype(stdexec::connect(stdexec::schedule(process::ObjectScheduler{}), DeletingReceiver{}));
        void* owner{};
        owner = new Operation(stdexec::connect(
            stdexec::schedule(process::objectScheduler(target)),
            DeletingReceiver{&result, &owner, [](void* p) noexcept { delete static_cast<Operation*>(p); }}
        ));
        stdexec::start(*static_cast<Operation*>(owner));
        assert(object::ObjectRuntime::instance().dispatchPending() == 1 && !owner && result.value == 1);
    }

    void blockingToOwner()
    {
        process::ExecutionRuntimeConfig config{1, 16, 16, {16}};
        config.blocking = process::BlockingSchedulerConfig{1, 16};
        auto execution = process::ExecutionRuntime::create(config);
        assert(execution);
        auto blocking = execution->blocking();
        assert(blocking);
        auto& runtime = object::ObjectRuntime::instance();
        const auto owner_thread = std::this_thread::get_id();
        Target target;
        Result result;
        std::atomic_bool worked{};
        auto work = [owner_thread, &worked]() noexcept
        {
            assert(std::this_thread::get_id() != owner_thread);
            worked.store(true, std::memory_order_release);
        };
        auto receive = [&target, &worked]() noexcept
        {
            assert(target.isOnAffinityThread() && worked.load(std::memory_order_acquire));
        };
        auto sender = stdexec::schedule(*blocking) | stdexec::then(work) |
                      stdexec::continues_on(process::objectScheduler(target)) | stdexec::then(receive);
        auto op = stdexec::connect(std::move(sender), Receiver{&result});
        stdexec::start(op);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!result.count() && std::chrono::steady_clock::now() < deadline)
        {
            (void)runtime.dispatchPending();
            std::this_thread::yield();
        }
        assert(result.value == 1 && result.thread == owner_thread);
    }

    void stress()
    {
        auto& runtime = object::ObjectRuntime::instance();
        using Operation = decltype(stdexec::connect(stdexec::schedule(process::ObjectScheduler{}), Receiver{}));
        std::mt19937 random{0x5eeda};
        std::size_t values{}, stopped{};
        // Bounded batches, actual target destruction and immediate slot reuse, no queue retries.
        for (unsigned batch{}; batch < 100; ++batch)
        {
            std::array<std::unique_ptr<Target>, 100> targets;
            std::array<Result, 100> results;
            std::array<std::unique_ptr<Operation>, 100> operations;
            std::array<bool, 100> destroyed{};
            for (unsigned i{}; i < 100; ++i)
            {
                targets[i] = std::make_unique<Target>();
                operations[i].reset(new Operation(
                    stdexec::connect(stdexec::schedule(process::objectScheduler(*targets[i])), Receiver{&results[i]})
                ));
                stdexec::start(*operations[i]);
                destroyed[i] = random() % 3 == 0;
                if (destroyed[i])
                {
                    targets[i].reset();
                }
            }
            assert(runtime.dispatchPending() == 100);
            for (unsigned i{}; i < 100; ++i)
            {
                assert(results[i].count() == 1 && results[i].stopped == static_cast<unsigned>(destroyed[i]));
                values += results[i].value;
                stopped += results[i].stopped;
            }
        }
        assert(values + stopped == 10000 && values && stopped && runtime.statistics().pending == 0);
        std::printf("O08: %zu value + %zu stopped = 10000, exactly once, no pending envelopes\n", values, stopped);
    }
} // namespace

int main()
{
    (void)object::ObjectRuntime::instance();
    lifetime();
    cancellationAndCapacity();
    threadAndBorrow();
    completionDestroysOperation();
    blockingToOwner();
    stress();
    std::puts("PASS O01-O08: lifetime, closure, capacity, stop, owner thread and stress");
}

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <latch>
#include <lux/engine/core/execution/RunLoop.hpp>
#include <memory>
#include <thread>

using lux::execution::RunLoop;

static void check(bool condition) noexcept
{
    if (!condition)
    {
        std::abort();
    }
}

static void scheduleAgain(RunLoop& loop, stdexec::counting_scope& scope, unsigned& count, bool& repeat)
{
    stdexec::spawn(
        stdexec::schedule(loop.scheduler()) | stdexec::then(
                                                  [&]() noexcept
                                                  {
                                                      ++count;
                                                      if (repeat)
                                                      {
                                                          scheduleAgain(loop, scope, count, repeat);
                                                      }
                                                  }
                                              ),
        scope.get_token()
    );
}

static void limitedConsumption()
{
    RunLoop loop;
    stdexec::counting_scope scope;
    unsigned count = 0;
    bool repeat = true;
    scheduleAgain(loop, scope, count, repeat);
    check(loop.executeReady(0) == 0 && count == 0);
    check(loop.executeReady(17) == 17 && count == 17 && loop.size() == 1);
    repeat = false;
    check(loop.executeReady(17) == 1 && count == 18 && loop.empty());
    scope.close();
    check(stdexec::sync_wait(scope.join()).has_value());
}

static void publisherTail()
{
    struct State final
    {
        std::latch published{1};
        std::latch finish_wakeup{1};
        unsigned destroyed = 0;
    } state;

    RunLoop loop{
        [](void* value) noexcept
        {
            auto& state = *static_cast<State*>(value);
            state.published.count_down();
            state.finish_wakeup.wait();
        },
        &state
    };

    struct Lifetime final
    {
        unsigned& destroyed;

        explicit Lifetime(unsigned& count) noexcept : destroyed(count) {}

        ~Lifetime()
        {
            ++destroyed;
        }
    };

    stdexec::counting_scope scope;
    std::thread producer(
        [&]
        {
            stdexec::spawn(
                stdexec::schedule(loop.scheduler()) |
                    stdexec::then([lifetime = std::make_unique<Lifetime>(state.destroyed)]() noexcept {}),
                scope.get_token()
            );
        }
    );
    const auto before_publication = loop.publicationEpoch();
    state.published.wait();
    check(loop.activePublishers() == 1);
    check(loop.executeReady(1) == 1 && state.destroyed == 1 && loop.empty());
    check(loop.activePublishers() == 1 && !loop.canWait(before_publication));
    state.finish_wakeup.count_down();
    producer.join();
    check(loop.activePublishers() == 0 && !loop.canWait(before_publication));
    check(loop.canWait(loop.publicationEpoch()));
    scope.close();
    check(stdexec::sync_wait(scope.join()).has_value());
}

struct Receiver final
{
    using receiver_concept = stdexec::receiver_t;
    unsigned* completed;
    std::thread::id thread;
    stdexec::inplace_stop_token stop;

    auto get_env() const noexcept
    {
        return stdexec::env{stdexec::prop{stdexec::get_stop_token, stop}};
    }

    void set_value() && noexcept
    {
        check(std::this_thread::get_id() == thread);
        ++*completed;
    }
};

static void standardSchedule()
{
    RunLoop loop;
    stdexec::inplace_stop_source stop;
    stop.request_stop();
    unsigned completed = 0;
    auto operation = stdexec::connect(
        stdexec::schedule(loop.scheduler()),
        Receiver{&completed, std::this_thread::get_id(), stop.get_token()}
    );
    std::thread producer([&] { stdexec::start(operation); });
    producer.join();
    check(completed == 0 && loop.size() == 1);
    check(loop.executeReady(1) == 1 && completed == 1);

    // A completed error survives the handoff even when the consumer's stop was requested.
    auto result = stdexec::just_error(37) | stdexec::continues_on(loop.scheduler()) |
                  stdexec::upon_error([&](int error) noexcept { check(error == 37); });
    auto failure =
        stdexec::connect(std::move(result), Receiver{&completed, std::this_thread::get_id(), stop.get_token()});
    stdexec::start(failure);
    check(completed == 1 && loop.executeReady(1) == 1 && completed == 2);

    // Stop between result production and consumption must preserve that result too.
    stdexec::inplace_stop_source late_stop;
    auto late = stdexec::connect(
        stdexec::just(71) | stdexec::continues_on(loop.scheduler()) |
            stdexec::then([](int value) noexcept { check(value == 71); }),
        Receiver{&completed, std::this_thread::get_id(), late_stop.get_token()}
    );
    stdexec::start(late);
    late_stop.request_stop();
    check(completed == 2 && loop.executeReady(1) == 1 && completed == 3);
}

static void concurrentPublication()
{
    constexpr unsigned threads = 8;
    constexpr unsigned per_thread = 128;
    RunLoop loop;
    stdexec::counting_scope scope;
    std::array<unsigned, threads> completed{};
    std::latch start{threads};
    std::array<std::thread, threads> producers;
    for (unsigned t = 0; t < threads; ++t)
    {
        producers[t] = std::thread(
            [&, t]
            {
                start.count_down();
                start.wait();
                for (unsigned i = 0; i < per_thread; ++i)
                {
                    stdexec::spawn(
                        stdexec::schedule(loop.scheduler()) |
                            stdexec::then([&, t, i]() noexcept { check(completed[t]++ == i); }),
                        scope.get_token()
                    );
                }
            }
        );
    }
    unsigned consumed = 0;
    while (consumed != threads * per_thread)
    {
        consumed += static_cast<unsigned>(loop.executeReady(7));
        std::this_thread::yield();
    }
    for (auto& producer : producers)
    {
        producer.join();
    }
    for (const auto count : completed)
    {
        check(count == per_thread);
    }
    check(loop.empty() && loop.activePublishers() == 0);
    scope.close();
    check(stdexec::sync_wait(scope.join()).has_value());
}

int main()
{
    limitedConsumption();
    publisherTail();
    standardSchedule();
    concurrentPublication();
    std::printf(
        "RunLoop=%zu Scheduler=%zu ScheduleSender=%zu; "
        "per-producer order, limited consumption, thread affinity, stop delivery and publisher tail passed\n",
        sizeof(RunLoop),
        sizeof(RunLoop::Scheduler),
        sizeof(RunLoop::ScheduleSender)
    );
}

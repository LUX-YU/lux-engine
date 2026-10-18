#include <lux/engine/core/execution/RunLoop.hpp>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <latch>
#include <thread>
#include <vector>

using lux::execution::RunLoop;

static void check(bool condition) noexcept
{
    if (!condition) std::abort();
}

struct CountingWork final : RunLoop::Work
{
    unsigned& count;
    RunLoop* repost;

    CountingWork(unsigned& output, RunLoop* loop = nullptr) noexcept
        : Work([](Work& value) noexcept {
              auto& self = static_cast<CountingWork&>(value);
              ++self.count;
              if (self.repost) self.repost->post(self);
          }), count(output), repost(loop)
    {}
};

static void budgetAndRepost()
{
    RunLoop loop;
    unsigned count = 0;
    CountingWork work{count, &loop};
    loop.post(work);
    check(loop.executeReady(0) == 0 && count == 0);
    check(loop.executeReady(17) == 17 && count == 17 && loop.size() == 1);
    work.repost = nullptr;
    check(loop.executeReady(17) == 1 && count == 18 && loop.empty());
}

static void publisherTail()
{
    struct State final
    {
        std::latch published{1};
        std::latch finish_wakeup{1};
        unsigned destroyed = 0;
    } state;
    RunLoop loop{[](void* value) noexcept {
        auto& state = *static_cast<State*>(value);
        state.published.count_down();
        state.finish_wakeup.wait();
    }, &state};
    struct OwnedWork final : RunLoop::Work
    {
        unsigned& destroyed;
        explicit OwnedWork(unsigned& count) noexcept
            : Work([](Work& work) noexcept { delete &static_cast<OwnedWork&>(work); }), destroyed(count)
        {}
        ~OwnedWork() { ++destroyed; }
    };
    auto* work = new OwnedWork{state.destroyed};
    std::thread producer([&] { loop.post(*work); });
    state.published.wait();
    check(loop.activePublishers() == 1);
    check(loop.executeReady(1) == 1 && state.destroyed == 1 && loop.empty());
    check(loop.activePublishers() == 1); // Empty work is not the end of the publisher's callback stack.
    state.finish_wakeup.count_down();
    producer.join();
    check(loop.activePublishers() == 0);
}

struct Receiver final
{
    using receiver_concept = stdexec::receiver_t;
    unsigned* completed;
    std::thread::id owner;
    stdexec::inplace_stop_token stop;

    auto get_env() const noexcept
    {
        return stdexec::env{stdexec::prop{stdexec::get_stop_token, stop}};
    }

    void set_value() && noexcept
    {
        check(std::this_thread::get_id() == owner);
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
        stdexec::schedule(loop.scheduler()), Receiver{&completed, std::this_thread::get_id(), stop.get_token()}
    );
    std::thread producer([&] { stdexec::start(operation); });
    producer.join();
    check(completed == 0 && loop.size() == 1);
    check(loop.executeReady(1) == 1 && completed == 1);

    // A completed error must survive the owner handoff even when the consumer's stop was requested.
    auto result = stdexec::just_error(37)
        | stdexec::continues_on(loop.scheduler())
        | stdexec::upon_error([&](int error) noexcept { check(error == 37); });
    auto failure = stdexec::connect(
        std::move(result), Receiver{&completed, std::this_thread::get_id(), stop.get_token()}
    );
    stdexec::start(failure);
    check(completed == 1 && loop.executeReady(1) == 1 && completed == 2);

    // Scope owns and destroys the connected scheduler node during terminal delivery.
    stdexec::counting_scope scope;
    stdexec::spawn(
        stdexec::schedule(loop.scheduler()) | stdexec::then([&]() noexcept { ++completed; }), scope.get_token()
    );
    check(loop.executeReady(1) == 1 && completed == 3);
    scope.close();
    check(stdexec::sync_wait(scope.join()).has_value());
}

static void concurrentPublication()
{
    constexpr unsigned threads = 8, per_thread = 128;
    RunLoop loop;
    std::array<unsigned, threads> completed{};
    struct OrderedWork final : RunLoop::Work
    {
        unsigned& consumed;
        unsigned index;
        OrderedWork(unsigned& count, unsigned value) noexcept
            : Work([](Work& work) noexcept {
                  auto& self = static_cast<OrderedWork&>(work);
                  check(self.consumed++ == self.index);
              }), consumed(count), index(value)
        {}
    };
    std::array<std::vector<std::unique_ptr<OrderedWork>>, threads> work;
    for (unsigned t = 0; t < threads; ++t)
        for (unsigned i = 0; i < per_thread; ++i)
            work[t].push_back(std::make_unique<OrderedWork>(completed[t], i));
    std::latch start{threads};
    std::array<std::thread, threads> producers;
    for (unsigned t = 0; t < threads; ++t)
        producers[t] = std::thread([&, t] {
            start.count_down();
            start.wait();
            for (auto& item : work[t]) loop.post(*item);
        });
    unsigned consumed = 0;
    while (consumed != threads * per_thread)
    {
        consumed += static_cast<unsigned>(loop.executeReady(7));
        std::this_thread::yield();
    }
    for (auto& producer : producers) producer.join();
    for (const auto count : completed) check(count == per_thread);
    check(loop.empty() && loop.activePublishers() == 0);
}

int main()
{
    budgetAndRepost();
    publisherTail();
    standardSchedule();
    concurrentPublication();
    std::printf(
        "RunLoop=%zu Work=%zu Scheduler=%zu ScheduleSender=%zu; "
        "FIFO, budget, owner affinity, stop delivery and publisher tail passed\n",
        sizeof(RunLoop), sizeof(RunLoop::Work), sizeof(RunLoop::Scheduler), sizeof(RunLoop::ScheduleSender)
    );
}

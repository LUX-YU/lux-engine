#include <cassert>
#include <cstdio>
#include <lux/engine/process/OnMain.hpp>
#include <string>
#include <thread>

using namespace lux::process;

struct DomainError final
{
    int reason;
};
struct Results final
{
    std::thread::id owner{std::this_thread::get_id()};
    int values{}, stopped{}, execution_errors{}, domain_errors{}, value{}, reason{};
};

struct Receiver final
{
    using receiver_concept = stdexec::receiver_t;
    Results *results;
    stdexec::env<> get_env() const noexcept
    {
        return {};
    }
    void set_value() && noexcept
    {
        assert(std::this_thread::get_id() == results->owner);
        ++results->values;
    }
    void set_value(int value) && noexcept
    {
        assert(std::this_thread::get_id() == results->owner);
        ++results->values;
        results->value = value;
    }
    void set_error(EExecutionError error) && noexcept
    {
        assert(std::this_thread::get_id() == results->owner);
        ++results->execution_errors;
        results->reason = static_cast<int>(error);
    }
    void set_error(DomainError error) && noexcept
    {
        assert(std::this_thread::get_id() == results->owner);
        ++results->domain_errors;
        results->reason = error.reason;
    }
    void set_stopped() && noexcept
    {
        assert(std::this_thread::get_id() == results->owner);
        ++results->stopped;
    }
};

struct Pending final
{
    void *operation{};
    void (*value)(void *, int) noexcept {};
    void (*error)(void *, DomainError) noexcept {};
    void (*stopped)(void *) noexcept {};
};

struct ManualSender final
{
    using sender_concept = stdexec::sender_t;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t(int), stdexec::set_error_t(DomainError),
                                       stdexec::set_stopped_t()>;
    Pending *pending;
    template <class R> struct Operation final
    {
        using operation_state_concept = stdexec::operation_state_t;
        Pending *pending;
        R receiver;
        void start() & noexcept
        {
            pending->operation = this;
            pending->value = [](void *owner, int value) noexcept
            {
                stdexec::set_value(std::move(static_cast<Operation *>(owner)->receiver), value);
            };
            pending->error = [](void *owner, DomainError error) noexcept
            {
                stdexec::set_error(std::move(static_cast<Operation *>(owner)->receiver), error);
            };
            pending->stopped = [](void *owner) noexcept
            {
                stdexec::set_stopped(std::move(static_cast<Operation *>(owner)->receiver));
            };
        }
    };
    template <class R> Operation<std::decay_t<R>> connect(R receiver) &&
    {
        return {pending, std::move(receiver)};
    }
};

template <class Env>
concept HasErrorCompletionScheduler =
    requires(Env env) { stdexec::get_completion_scheduler<stdexec::set_error_t>(env); };

int main()
{
    auto runtime = ExecutionRuntime::create({1, 8, 2, {8}, BlockingSchedulerConfig{1, 8}});
    assert(runtime);
    using RetainedSender = decltype(deliverOnMain(*runtime, ManualSender{}));
    static_assert(!HasErrorCompletionScheduler<stdexec::env_of_t<RetainedSender>>);
    Results wrong_thread;
    Pending unstarted;
    auto wrongly_started = stdexec::connect(deliverOnMain(*runtime, ManualSender{&unstarted}), Receiver{&wrong_thread});
    std::thread initiator(
        [&]
        {
            wrong_thread.owner = std::this_thread::get_id();
            stdexec::start(wrongly_started);
        });
    initiator.join();
    assert(!unstarted.operation && wrong_thread.execution_errors == 1 &&
           wrong_thread.reason == int(EExecutionError::WRONG_THREAD));
    Results ordinary, retained, rejected;
    Pending pending;
    auto first = stdexec::connect(stdexec::schedule(runtime->main()), Receiver{&ordinary});
    auto completion = stdexec::connect(deliverOnMain(*runtime, ManualSender{&pending}), Receiver{&retained});
    auto overflow = stdexec::connect(stdexec::schedule(runtime->main()), Receiver{&rejected});
    stdexec::start(first);
    stdexec::start(completion);
    assert(pending.operation);
    stdexec::start(overflow);
    assert(rejected.execution_errors == 1 && rejected.reason == int(EExecutionError::CAPACITY_EXCEEDED));
    std::thread producer(
        [&]
        {
            pending.value(pending.operation, 42);
        });
    producer.join();
    assert(retained.values == 0);
    runtime->requestStop();
    assert(*runtime->drainMain(1) == 1 && ordinary.stopped == 1 && retained.values == 0);
    auto early_join = runtime->join();
    assert(!early_join && early_join.error() == EExecutionError::MAIN_QUEUE_NOT_DRAINED);
    assert(*runtime->drainMain(1) == 1 && retained.values == 1 && retained.value == 42 && retained.stopped == 0);
    assert(*runtime->drainMain(1) == 0 && runtime->join());

    auto second = ExecutionRuntime::create({1, 8, 1, {8}, BlockingSchedulerConfig{1, 8}});
    assert(second);
    Results occupied, unavailable, failure;
    Pending never_started, delayed;
    auto occupied_operation = stdexec::connect(stdexec::schedule(second->main()), Receiver{&occupied});
    stdexec::start(occupied_operation);
    auto unavailable_operation =
        stdexec::connect(deliverOnMain(*second, ManualSender{&never_started}), Receiver{&unavailable});
    stdexec::start(unavailable_operation);
    assert(!never_started.operation && unavailable.execution_errors == 1);
    assert(*second->drainMain(1) == 1);
    auto delayed_operation = stdexec::connect(deliverOnMain(*second, ManualSender{&delayed}), Receiver{&failure});
    stdexec::start(delayed_operation);
    second->requestStop();
    auto outstanding = second->join();
    assert(!outstanding && outstanding.error() == EExecutionError::MAIN_QUEUE_NOT_DRAINED);
    delayed.error(delayed.operation, {73});
    assert(*second->drainMain(1) == 1 && failure.domain_errors == 1 && failure.reason == 73 && failure.stopped == 0);
    assert(second->join());
    std::puts("PASS owner-completion: FIFO budget=1 value=42 domain-error=73 stopped-does-not-replace-result "
              "admission-before-start=1");
}

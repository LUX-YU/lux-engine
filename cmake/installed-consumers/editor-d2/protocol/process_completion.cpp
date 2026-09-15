#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <lux/engine/editor/project/ProjectPublication.hpp>
#include <lux/engine/process/OnMain.hpp>
#include <semaphore>
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
            { stdexec::set_value(std::move(static_cast<Operation *>(owner)->receiver), value); };
            pending->error = [](void *owner, DomainError error) noexcept
            { stdexec::set_error(std::move(static_cast<Operation *>(owner)->receiver), error); };
            pending->stopped = [](void *owner) noexcept
            { stdexec::set_stopped(std::move(static_cast<Operation *>(owner)->receiver)); };
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

void fileCompletion()
{
    const auto root = std::filesystem::current_path() / "process-file-evidence" /
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root);
    const auto refused_file = root / "not-admitted.txt";
    const auto completed_file = root / "accepted.txt";
    constexpr std::string_view payload = "one admitted file effect, original value 42\n";
    auto runtime = ExecutionRuntime::create({1, 8, 1, {8}, BlockingSchedulerConfig{1, 8}});
    assert(runtime);
    Results occupied, refused;
    std::atomic_uint writes{};
    auto occupy = stdexec::connect(stdexec::schedule(runtime->main()), Receiver{&occupied});
    stdexec::start(occupy);
    auto upstream = stdexec::then(stdexec::schedule(*runtime->blocking()),
                                  [&]() noexcept
                                  {
                                      std::ofstream file(refused_file, std::ios::binary);
                                      file << payload;
                                      assert(file.good());
                                      ++writes;
                                      return 42;
                                  });
    auto rejected = stdexec::connect(deliverOnMain(*runtime, std::move(upstream)), Receiver{&refused});
    stdexec::start(rejected);
    assert(refused.execution_errors == 1 && refused.reason == int(EExecutionError::CAPACITY_EXCEEDED));
    assert(writes == 0 && !std::filesystem::exists(refused_file));
    assert(*runtime->drainMain(1) == 1);
    runtime->requestStop();
    assert(runtime->join());

    auto executing = ExecutionRuntime::create({1, 8, 2, {8}, BlockingSchedulerConfig{1, 8}});
    assert(executing);
    std::binary_semaphore effect_done{0}, release_result{0};
    Results result, filler, pressure;
    auto effect = stdexec::then(stdexec::schedule(*executing->blocking()),
                                [&]() noexcept
                                {
                                    {
                                        std::ofstream file(completed_file, std::ios::binary);
                                        file << payload;
                                        file.flush();
                                        assert(file.good());
                                    }
                                    ++writes;
                                    effect_done.release();
                                    release_result.acquire();
                                    return 42;
                                });
    auto operation = stdexec::connect(deliverOnMain(*executing, std::move(effect)), Receiver{&result});
    stdexec::start(operation);
    effect_done.acquire();
    auto fill = stdexec::connect(stdexec::schedule(executing->main()), Receiver{&filler});
    auto overflow = stdexec::connect(stdexec::schedule(executing->main()), Receiver{&pressure});
    stdexec::start(fill);
    stdexec::start(overflow);
    assert(pressure.execution_errors == 1 && pressure.reason == int(EExecutionError::CAPACITY_EXCEEDED));
    executing->requestStop();
    release_result.release();
    while (!result.values)
    {
        assert(executing->drainMain(1));
        std::this_thread::yield();
    }
    while (*executing->drainMain(1))
    {
    }
    assert(executing->join());
    assert(result.values == 1 && result.value == 42 && !result.stopped && !result.execution_errors && writes == 1);
    std::ifstream input(completed_file, std::ios::binary);
    const std::string observed(std::istreambuf_iterator<char>{input}, {});
    assert(observed == payload && !std::filesystem::exists(refused_file));
    const auto digest = lux::editor::projectContentDigest(std::as_bytes(std::span{observed.data(), observed.size()}));
    std::printf("PASS real-file Main reservation: refused_writes=0 admitted_writes=1 bytes=%zu value=42 "
                "deliveries=1 stopped=0 sha256=%s path=%s\n",
                observed.size(), digest.c_str(), completed_file.string().c_str());
}

int main()
{
    fileCompletion();
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
    std::thread producer([&] { pending.value(pending.operation, 42); });
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

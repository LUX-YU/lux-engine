#include <lux/engine/editor/tasks/TaskMonitor.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <cassert>
#include <algorithm>
#include <chrono>
#include <thread>
#include <type_traits>
#include <cstdio>
using namespace lux;
using namespace lux::editor;
namespace
{
    template <class T> auto take(T value)
    {
        assert(value);
        return std::move(*value);
    }
}
static_assert(!std::is_copy_constructible_v<tasks::TaskMonitor>);
static_assert(!std::is_move_constructible_v<tasks::TaskMonitor>);
int main()
{
    auto execution =
        take(process::ExecutionRuntime::create({.cpu_concurrency = 1, .cpu_queue_capacity = 32, .timer = {16}}));
    auto messages = take(object::ObjectMessageQueue::create(64));
    auto delayed = take(object::ObjectMessageQueue::create(1));
    object::LuxObject receiver(delayed.dispatcherRef());
    tasks::TaskMonitor monitor(messages.dispatcherRef(), execution);
    unsigned notifications{};
    auto connection = take(object::LuxObject::connect(
        &monitor,
        &tasks::TaskMonitor::changed,
        &receiver,
        [&](std::uint64_t) noexcept { ++notifications; },
        object::EDelivery::QUEUED
    ));
    object::LuxObject second(messages.dispatcherRef());
    unsigned direct_notifications{};
    auto direct = take(object::LuxObject::connect(
        &monitor,
        &tasks::TaskMonitor::changed,
        &second,
        [&](std::uint64_t revision) noexcept {
            assert(revision == monitor.revision());
            ++direct_notifications;
        },
        object::EDelivery::DIRECT
    ));
    bool completed{};
    auto task = take(execution.submit(
        {"monitor", "P10Q"},
        [timer = execution.timer()](process::TaskReporter) noexcept {
            return stdexec::upon_error(
                stdexec::then(
                    timer.after(std::chrono::hours(1)),
                    []() noexcept -> cxx::expected<int, process::ETimerError> { return 1; }
                ),
                [](process::ETimerError error) noexcept -> cxx::expected<int, process::ETimerError> {
                    return cxx::unexpected(error);
                }
            );
        },
        [&](auto&& result) noexcept {
            assert(!result);
            completed = true;
        }
    ));
    take(execution.dispatchTaskEvents());
    assert(monitor.dispatchChanges().queued == 1);
    assert(direct_notifications == 1);
    const auto consumer_a = monitor.snapshot();
    const auto consumer_b = monitor.snapshot();
    assert(consumer_a.get() == consumer_b.get());
    const auto snapshot = monitor.snapshot();
    for (int i{}; i != 1000; ++i)
        assert(monitor.snapshot().get() == snapshot.get());
    assert(monitor.requestCancel(task.id()));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!completed)
    {
        assert(std::chrono::steady_clock::now() < deadline);
        take(execution.collectCompletions());
        take(execution.dispatchTaskEvents());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    take(execution.dispatchTaskEvents());
    const auto lost = monitor.dispatchChanges();
    assert(lost.full == 1 && notifications == 0); // No retry of partially delivered broadcasts.
    const auto terminal = monitor.snapshot();
    assert(terminal.get() != snapshot.get());
    const auto found = std::ranges::find(*terminal, task.id(), &process::TaskInfo::id);
    assert(found != terminal->end() && found->finished && found->state == process::ETaskState::CANCELLED);
    assert(delayed.dispatchPending() == 1 && notifications == 1);
    delayed.close();
    direct.disconnect();
    bool next_done{};
    auto next = take(execution.submit(
        {"after subscribers detached", "P10Q"},
        [cpu = execution.cpu()](process::TaskReporter) noexcept {
            return stdexec::then(stdexec::schedule(cpu), []() noexcept -> cxx::expected<int, int> { return 1; });
        },
        [&](auto&& result) noexcept {
            assert(result);
            next_done = true;
        }
    ));
    take(execution.dispatchTaskEvents());
    assert(monitor.dispatchChanges().closed == 1);
    while (!next_done)
    {
        assert(std::chrono::steady_clock::now() < deadline);
        take(execution.collectCompletions());
        take(execution.dispatchTaskEvents());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    take(execution.dispatchTaskEvents());
    static_cast<void>(monitor.dispatchChanges());
    assert(execution.taskInfo(next.id())->state == process::ETaskState::SUCCEEDED);
    std::puts(
        "XL13 actual CPU Runtime: two subscribers share rows; FULL/CLOSED preserve terminal facts; detach does not "
        "cancel observer/tasks"
    );
}

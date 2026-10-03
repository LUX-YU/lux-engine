#include <lux/engine/editor/tasks/TaskView.hpp>
#include <lux/engine/editor/desktop/ViewHost.hpp>
#include <lux/engine/ui/Root.hpp>
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
} // namespace
static_assert(!std::is_copy_constructible_v<tasks::TaskMonitor>);
static_assert(!std::is_move_constructible_v<tasks::TaskMonitor>);
int main()
{
    auto execution =
        take(process::ExecutionRuntime::create({.cpu_concurrency = 1, .cpu_queue_capacity = 32, .timer = {16}}));
    auto messages = take(object::ObjectMessageQueue::create(64));
    auto delayed = take(object::ObjectMessageQueue::create(1));
    object::LuxObject receiver(delayed.dispatcherRef());
    auto root = take(ui::Root::create(messages.dispatcherRef(), {.docking = false}));
    tasks::TaskMonitor monitor(messages.dispatcherRef(), execution);
    desktop::ViewHost host(*root);
    unsigned notifications{};
    auto connection = take(object::LuxObject::connect(
        &monitor,
        &tasks::TaskMonitor::changed,
        &receiver,
        [&](std::uint64_t) noexcept { ++notifications; },
        object::EDelivery::QUEUED
    ));
    auto factory = tasks::makeTaskViewFactory(monitor);
    auto peer = tasks::makeTaskViewFactory(monitor);
    assert(&factory->descriptor() == &peer->descriptor());
    auto factories = take(views::ViewFactorySnapshot::create({factory}));
    const auto create = [&](const char* id)
    {
        return take(factories.prepare(
            views::ViewTypeId{"lux.editor.tasks"},
            {messages.dispatcherRef(),
             ui::PaneId{id},
             contracts::CodeLease::builtin(),
             cxx::typeToken<std::monostate>(),
             std::make_shared<const std::monostate>()}
        ));
    };
    auto first = create("first");
    auto second = create("second");
    assert(root->panes().empty());
    auto* a = static_cast<tasks::TaskView*>(first.pane());
    auto* b = static_cast<tasks::TaskView*>(second.pane());
    const auto aid = take(host.adopt(first, views::ViewRestoreKey{"first"})).id;
    const auto bid = take(host.adopt(second, views::ViewRestoreKey{"second"})).id;
    bool completed{};
    auto task = take(execution.submit(
        {"monitor", "P10Q"},
        [timer = execution.timer()](process::TaskReporter) noexcept
        {
            return stdexec::upon_error(
                stdexec::then(
                    timer.after(std::chrono::hours(1)),
                    []() noexcept -> cxx::expected<int, process::ETimerError> { return 1; }
                ),
                [](process::ETimerError error) noexcept -> cxx::expected<int, process::ETimerError>
                { return cxx::unexpected(error); }
            );
        },
        [&](auto&& result) noexcept
        {
            assert(!result);
            completed = true;
        }
    ));
    take(execution.dispatchTaskEvents());
    assert(monitor.dispatchChanges().queued == 1);
    assert(root->update({{640, 480}, .016F}, nullptr));
    assert(a->tasks().rows().data() == b->tasks().rows().data());
    const auto snapshot = monitor.snapshot();
    for (int i{}; i != 1000; ++i)
        assert(monitor.snapshot().get() == snapshot.get());
    a->tasks().requestCancel(task.id());
    assert(root->update({{640, 480}, .016F}, nullptr));
    assert(a->tasks().rejectedCancellations().empty());
    assert(host.close(aid) && host.close(bid) && take(host.drain()).completed == 2);
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
    bool next_done{};
    auto next = take(execution.submit(
        {"after views closed", "P10Q"},
        [cpu = execution.cpu()](process::TaskReporter) noexcept
        {
            return stdexec::then(stdexec::schedule(cpu), []() noexcept -> cxx::expected<int, int> { return 1; });
        },
        [&](auto&& result) noexcept
        {
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
    std::puts("XQ05 actual Runtime: two views share rows; FULL/CLOSED preserve terminal facts; last view does not "
              "cancel observer/tasks");
}

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/editor/tasks/TaskView.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/ui/Root.hpp>
#include <thread>
#include <type_traits>
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
    services::ServiceRegistry services(messages.dispatcherRef());
    auto scope = take(services.createScope());
    assert(scope.provide(services::ServiceNameView{"lux.editor.tasks.monitor"}, monitor));
    desktop::UiRegistry windows(messages.dispatcherRef(), services);
    unsigned notifications{};
    auto connection = take(object::LuxObject::connect(
        &monitor,
        &tasks::TaskMonitor::changed,
        &receiver,
        [&](std::uint64_t) noexcept { ++notifications; },
        object::EDelivery::QUEUED
    ));
    auto entry = desktop::UiEntry::bind<tasks::kTaskView>(object::CodeLease::builtin());
    auto peer = desktop::UiEntry::bind<tasks::kTaskView>(object::CodeLease::builtin());
    assert(&entry->descriptor() == &peer->descriptor());
    auto catalog = take(desktop::UiCatalog::prepare({std::move(entry)}));
    assert(windows.publish(catalog));
    const auto factory = take(catalog.at(0));
    const auto create = [&](const char* id)
    { return take(windows.create(factory, scope, {messages.dispatcherRef(), ui::PaneId{id}, {}, {}})); };
    auto rejected =
        windows.create(factory, scope, {messages.dispatcherRef(), ui::PaneId{"invalid"}, {}, {1, {std::byte{1}}}});
    assert(!rejected && rejected.error().code == desktop::EUiError::INVALID_CONFIGURATION);
    auto first = create("first");
    auto second = create("second");
    assert(root->panes().empty());
    auto* a = static_cast<tasks::TaskView*>(first.get());
    auto* b = static_cast<tasks::TaskView*>(second.get());
    assert(root->addSubPane(std::move(first)) && root->addSubPane(std::move(second)));
    const std::array handles{take(root->identify(*a)), take(root->identify(*b))};
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
    {
        assert(monitor.snapshot().get() == snapshot.get());
    }
    a->tasks().requestCancel(task.id());
    assert(root->update({{640, 480}, .016F}, nullptr));
    assert(a->tasks().rejectedCancellations().empty());
    auto close = take(windows.prepareClose(*root, handles));
    assert(root->commit(close));
    assert(!root->findPane(handles[0]) && !root->findPane(handles[1]));
    assert(messages.collectRetired() == 2);
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
    auto reopened = create("reopened");
    auto* observer = static_cast<tasks::TaskView*>(reopened.get());
    assert(root->addSubPane(std::move(reopened)));
    assert(root->update({{640, 480}, .016F}, nullptr));
    const auto current = monitor.snapshot();
    assert(observer->tasks().rows().data() == current->data());
    const std::array reopened_id{take(root->identify(*observer))};
    auto closed_again = take(windows.prepareClose(*root, reopened_id));
    assert(root->commit(closed_again) && messages.collectRetired() == 1);
    assert(scope.release() && scope.drained() && services.drained());
    std::puts("XQ05 actual Runtime: two views share rows; FULL/CLOSED preserve terminal facts; last view does not "
              "cancel observer/tasks");
}

#include "TestChecks.hpp"
#include <lux/engine/editor/EditHistoryController.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/ui/CommandRouter.hpp>

using namespace lux::editor;
using namespace lux::editor::editing;
using namespace lux::editor::editing::test;
namespace
{
    class Proxy final : public EditHistoryTarget
    {
    public:
        Session& session;
        std::optional<EHistoryActionAvailability> availability;
        std::function<void()> on_view;
        bool wrong_identity{}, wrong_result{};
        explicit Proxy(Session& value) : session(value)
        {
        }
        HistoryId historyId() const noexcept override
        {
            return session.historyId();
        }
        EditResult<HistoryTargetView> historyView() const noexcept override
        {
            if (on_view)
            {
                on_view();
            }
            auto value = session.historyView();
            if (value && availability)
            {
                value->undo = value->redo = *availability;
            }
            if (value && wrong_identity)
            {
                value->history.history = {};
            }
            return value;
        }
        EditResult<HistoryTargetResult> undo() noexcept override
        {
            auto result = session.undo();
            if (result && wrong_result)
            {
                result->content.current.history = {};
            }
            return result;
        }
        EditResult<HistoryTargetResult> redo() noexcept override
        {
            return session.redo();
        }
    };
    class FailureSink final : public lux::object::Object<FailureSink>
    {
    public:
        using Object::Object;
        std::size_t calls{};
        EditHistoryActionFailure last;
        std::function<void()> on_failure;
        void receive(const EditHistoryActionFailure& failure) noexcept
        {
            last = failure;
            ++calls;
            if (on_failure)
            {
                on_failure();
            }
        }
    };
    auto route(std::size_t capacity = 4U)
    {
        auto value = ActiveEditHistory::create(capacity);
        assert(value);
        return std::move(*value);
    }
    auto registration(ActiveEditHistory& route, EditHistoryTarget& target)
    {
        auto value = route.registerTarget(target);
        assert(value);
        return std::move(*value);
    }
    void routeCases()
    {
        check(
            "R01",
            []
            {
                TextSession a;
                RecordSession b;
                change(a, "B");
                execute(b, b.patch(1, {}, 4));
                auto r = route();
                auto ta = registration(*r, a);
                auto tb = registration(*r, b);
                assert(!r->view()->has_target && r->activate(ta.handle()) && r->undo());
                assert(a.text() == "alpha" && b.records().at(1) == 4);
                assert(r->redo() && r->activate(tb.handle()) && r->undo());
                assert(b.records().empty() && a.text() == "B" && r->redo());
            }
        );
        check(
            "R02",
            []
            {
                TextSession a;
                auto r = route();
                auto t = registration(*r, a);
                expectError(r->undo(), EEditError::NO_ACTIVE_TARGET);
                expectError(r->redo(), EEditError::NO_ACTIVE_TARGET);
                assert(!r->view()->has_target && r->activate(t.handle()) && r->deactivate());
                expectError(r->undo(), EEditError::NO_ACTIVE_TARGET);
                assert(a.stats.target_calls == 0U);
            }
        );
        check(
            "R03",
            []
            {
                expectError(ActiveEditHistory::create(0U), EEditError::INVALID_LIMITS);
                expectError(
                    ActiveEditHistory::create((std::numeric_limits<std::size_t>::max)()), EEditError::INVALID_LIMITS
                );
                TextSession a, b;
                Proxy duplicate(a);
                auto r = route(1U);
                auto t = registration(*r, a);
                expectError(r->registerTarget(a), EEditError::DUPLICATE_TARGET);
                expectError(r->registerTarget(duplicate), EEditError::DUPLICATE_TARGET);
                expectError(r->registerTarget(b), EEditError::TARGET_CAPACITY);
            }
        );
        check(
            "R04",
            []
            {
                TextSession a, b;
                Proxy p(a);
                auto r = route(), foreign = route();
                auto ta = registration(*r, p);
                auto tb = registration(*foreign, b);
                assert(r->activate(ta.handle()));
                p.on_view = [] { assert(false); };
                expectError(r->activate(tb.handle()), EEditError::STALE_TARGET);
                assert(*r->activeTarget() == ta.handle());
                p.on_view = {};
            }
        );
        check(
            "R05",
            []
            {
                TextSession a, b;
                auto r = route(1U);
                auto ta = registration(*r, a);
                const auto stale = ta.handle();
                assert(r->activate(stale) && ta.reset() && !r->activeTarget()->valid());
                auto tb = registration(*r, b);
                assert(tb.handle() != stale && r->activate(tb.handle()));
                expectError(r->activate(stale), EEditError::STALE_TARGET);
                assert(*r->activeTarget() == tb.handle());
            }
        );
        check(
            "R06",
            []
            {
                TextSession a, b;
                change(a, "B");
                change(b, "C");
                Proxy p(a);
                auto r = route();
                auto ta = registration(*r, p);
                auto tb = registration(*r, b);
                assert(r->activate(ta.handle()));
                for (const auto pair :
                     {std::pair(EHistoryActionAvailability::BUSY, EEditError::BUSY),
                      std::pair(EHistoryActionAvailability::BLOCKED, EEditError::BLOCKED_BY_HOST),
                      std::pair(EHistoryActionAvailability::CLOSED, EEditError::CLOSED)})
                {
                    p.availability = pair.first;
                    expectError(r->undo(), pair.second);
                    expectError(r->redo(), pair.second);
                }
                p.availability.reset();
                p.wrong_identity = true;
                expectError(r->undo(), EEditError::CONTRACT_VIOLATION);
                assert(a.stats.target_calls == 0U && b.stats.target_calls == 0U);
            }
        );
        check(
            "R07",
            []
            {
                TextSession a;
                change(a, "B");
                auto r = route();
                auto t = registration(*r, a);
                assert(r->activate(t.handle()));
                a.preview = true;
                const Snapshot before(a);
                const auto result = r->undo();
                assert(result && result->outcome == EHistoryTargetOutcome::TRANSIENT_CANCELLED && !a.preview);
                assert(before == Snapshot(a) && r->undo() && a.text() == "alpha");
            }
        );
        check(
            "R08",
            []
            {
                TextSession a;
                auto r = route();
                auto t = registration(*r, a);
                assert(r->activate(t.handle()));
                a.preview = true;
                expectError(r->redo(), EEditError::BLOCKED_BY_HOST);
                assert(a.preview);
            }
        );
        check(
            "R09",
            []
            {
                TextSession a;
                change(a, "B");
                auto r = route();
                auto t = registration(*r, a);
                assert(r->activate(t.handle()));
                const auto handle = t.handle();
                a.callback = [&](EStage stage)
                {
                    if (stage == EStage::TARGET)
                    {
                        expectError(r->activate(handle), EEditError::BUSY);
                        expectError(t.reset(), EEditError::BUSY);
                        expectError(r->close(), EEditError::BUSY);
                        expectError(r->registerTarget(a), EEditError::BUSY);
                        assert(t.handle() == handle && *r->activeTarget() == handle);
                    }
                };
                assert(r->undo());
                a.callback = {};
                assert(t.reset());
            }
        );
        check(
            "R10",
            []
            {
                TextSession a, b;
                auto r = route();
                auto ta = registration(*r, a);
                auto tb = registration(*r, b);
                const auto ha = ta.handle(), hb = tb.handle();
                HistoryTargetRegistration moved(std::move(ta));
                assert(!ta.handle().valid() && moved.handle() == ha);
                moved = std::move(moved);
                assert(moved.handle() == ha);
                moved = std::move(tb);
                assert(!tb.handle().valid() && moved.handle() == hb);
                expectError(r->activate(ha), EEditError::STALE_TARGET);
                assert(r->activate(hb));
            }
        );
        check(
            "R11",
            []
            {
                auto a = std::make_unique<TextSession>();
                change(*a, "B");
                auto r = route();
                auto t = registration(*r, *a);
                const auto handle = t.handle();
                assert(r->activate(handle));
                a->callback = [&](EStage stage)
                {
                    if (stage == EStage::TARGET)
                    {
                        expectError(t.reset(), EEditError::BUSY);
                    }
                };
                assert(r->undo() && t.handle() == handle);
                a->callback = {};
                assert(t.reset());
                a.reset();
                expectError(r->activate(handle), EEditError::STALE_TARGET);
            }
        );
        check(
            "R12",
            []
            {
                TextSession a;
                HistoryTargetRegistration late;
                {
                    auto r = route();
                    late = registration(*r, a);
                    assert(r->close());
                }
                assert(late.reset() && !late.handle().valid() && a.stats.target_calls == 0U);
            }
        );
        check(
            "R13",
            []
            {
                TextSession a;
                auto r = route();
                auto t = registration(*r, a);
                const auto handle = t.handle();
                std::thread foreign(
                    [&]
                    {
                        expectError(r->activate(handle), EEditError::WRONG_THREAD);
                        expectError(r->view(), EEditError::WRONG_THREAD);
                        expectError(r->activeTarget(), EEditError::WRONG_THREAD);
                        expectError(t.reset(), EEditError::WRONG_THREAD);
                        expectError(r->registerTarget(a), EEditError::WRONG_THREAD);
                        expectError(r->close(), EEditError::WRONG_THREAD);
                    }
                );
                foreign.join();
                assert(t.handle() == handle && t.reset());
            }
        );
    }
    void commandCases()
    {
        lux::object::ObjectMessageQueue queue;
        TextSession a;
        RecordSession b;
        change(a, "B");
        execute(b, b.patch(1, {}, 4));
        auto histories = route();
        auto ta = registration(*histories, a);
        auto tb = registration(*histories, b);
        EditHistoryController controller(queue.dispatcherRef(), *histories);
        FailureSink sink(queue.dispatcherRef());
        FailureSink queued(queue.dispatcherRef());
        auto observation =
            controller.observe<EditHistoryController::failed, &FailureSink::receive, lux::object::EDelivery::DIRECT>(
                sink
            );
        auto queued_observation =
            controller.observe<EditHistoryController::failed, &FailureSink::receive, lux::object::EDelivery::QUEUED>(
                queued
            );
        assert(observation && queued_observation);
        lux::ui::CommandRouter commands;
        const auto undo = *commands.defineCommand({lux::ui::UiCommandId{"edit.undo"}, "Undo"});
        const auto redo = *commands.defineCommand({lux::ui::UiCommandId{"edit.redo"}, "Redo"});
        auto bu = commands.bindGlobal<&EditHistoryController::undo, &EditHistoryController::canUndo>(undo, controller);
        auto br = commands.bindGlobal<&EditHistoryController::redo, &EditHistoryController::canRedo>(redo, controller);
        assert(bu && br);
        check(
            "R14",
            [&]
            {
                assert(histories->activate(ta.handle()) && commands.state(undo).enabled);
                assert(controller.undoLabel() == "edit");
                assert(commands.invoke(undo) == lux::ui::ECommandDispatchResult::EXECUTED && a.text() == "alpha");
                assert(
                    commands.state(redo).enabled && commands.invoke(redo) == lux::ui::ECommandDispatchResult::EXECUTED
                );
                assert(histories->activate(tb.handle()));
                assert(commands.invoke(undo) == lux::ui::ECommandDispatchResult::EXECUTED && b.records().empty());
                assert(commands.invoke(redo) == lux::ui::ECommandDispatchResult::EXECUTED && b.records().at(1) == 4);
            }
        );
        check(
            "R15",
            [&]
            {
                assert(histories->activate(ta.handle()) && commands.state(undo).enabled);
                const auto calls = a.stats.target_calls;
                a.blocked = true;
                const auto failures = sink.calls;
                assert(commands.invoke(undo) == lux::ui::ECommandDispatchResult::DISABLED);
                assert(sink.calls == failures && calls == a.stats.target_calls);
                controller.undo();
                assert(sink.calls == failures + 1U && sink.last.target == ta.handle());
                assert(sink.last.failure.code == EEditError::BLOCKED_BY_HOST);
                a.blocked = false;
                queue.dispatchPending();
            }
        );
        check(
            "R16",
            [&]
            {
                const auto failures = sink.calls;
                a.reject_prepare = true;
                const Snapshot before(a);
                sink.on_failure = [&] { assert(histories->activate(tb.handle())); };
                assert(commands.invoke(undo) == lux::ui::ECommandDispatchResult::EXECUTED);
                assert(before == Snapshot(a) && sink.calls == failures + 1U && sink.last.target == ta.handle());
                assert(sink.last.failure.code == EEditError::PRECONDITION_FAILED);
                sink.on_failure = {};
                assert(queue.dispatchPending() == 1U && queued.last.target == ta.handle());
                assert(queued.last.failure.code == EEditError::PRECONDITION_FAILED);
                a.reject_prepare = false;
                assert(histories->activate(ta.handle()));
                assert(commands.invoke(undo) == lux::ui::ECommandDispatchResult::EXECUTED && a.text() == "alpha");
            }
        );
        check(
            "R17",
            [&]
            {
                assert(histories->activate(tb.handle()));
                const auto active = *histories->activeTarget();
                assert(commands.label(undo) == "Undo" && commands.state(undo).enabled);
                assert(*histories->activeTarget() == active);
                assert(commands.invoke(undo) == lux::ui::ECommandDispatchResult::EXECUTED && b.records().empty());
            }
        );
    }
} // namespace
int main()
{
    routeCases();
    commandCases();
    std::cout << "EDITOR_ROUTING PASS\n";
}

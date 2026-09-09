#include "EditingFixtures.hpp"
#include <lux/engine/editor/ui/actions/HistoryActions.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/ui/CommandRouter.hpp>
#include <cstdio>
#include <thread>

namespace
{
    using namespace lux::editor;
    using namespace editing;
    using namespace editing::test;
    class FailureSink final : public lux::object::Object<FailureSink>
    {
    public:
        using Object::Object;
        std::size_t calls{};
        ui::HistoryActionFailure last;
        void receive(const ui::HistoryActionFailure &failure) noexcept
        {
            ++calls;
            last = failure;
        }
    };
} // namespace
int main()
{
    lux::meta::ReflectionRegistry::initRegistry();
    {
        lux::object::ObjectMessageQueue messages;
        TextSession text;
        RecordSession record;
        execute(text, text.replace(0, text.text(), "changed"));
        execute(record, record.patch(1, {}, 4));
        auto made = ui::ActiveEditHistory::create(4);
        assert(made);
        auto histories = std::move(*made);
        auto a = histories->registerTarget(text);
        auto b = histories->registerTarget(record);
        assert(a && b);
        auto second_registration = histories->retainTarget(text);
        assert(second_registration && second_registration->handle() == a->handle());
        assert(!histories->registerTarget(text));
        assert(histories->activate(a->handle()));
        assert(a->reset());
        assert(histories->view(second_registration->handle()));
        a = std::move(second_registration);
        ui::HistoryActions text_actions(messages.dispatcherRef(), text);
        ui::HistoryActions record_actions(messages.dispatcherRef(), record);
        ui::HistoryActions second_text_pane(messages.dispatcherRef(), text);
        ui::HistoryMenuActions menu(messages.dispatcherRef(), *histories);
        FailureSink direct(messages.dispatcherRef()), queued(messages.dispatcherRef());
        auto signal =
            text_actions.observe<ui::HistoryActions::failed, &FailureSink::receive, lux::object::EDelivery::DIRECT>(
                direct);
        auto deferred =
            text_actions.observe<ui::HistoryActions::failed, &FailureSink::receive, lux::object::EDelivery::QUEUED>(
                queued);
        auto menu_signal =
            menu.observe<ui::HistoryMenuActions::failed, &FailureSink::receive, lux::object::EDelivery::DIRECT>(direct);
        assert(signal && deferred && menu_signal);
        lux::ui::CommandRouter commands;
        const auto tu = *commands.defineCommand({lux::ui::UiCommandId{"test.text.undo"}, "Undo text"});
        const auto tr = *commands.defineCommand({lux::ui::UiCommandId{"test.text.redo"}, "Redo text"});
        const auto ru = *commands.defineCommand({lux::ui::UiCommandId{"test.record.undo"}, "Undo record"});
        const auto mu = *commands.defineCommand({lux::ui::UiCommandId{"test.menu.undo"}, "Window Undo"});
        auto bt = commands.bindGlobal<&ui::HistoryActions::undo, &ui::HistoryActions::canUndo>(tu, text_actions);
        auto br = commands.bindGlobal<&ui::HistoryActions::redo, &ui::HistoryActions::canRedo>(tr, text_actions);
        auto bb = commands.bindGlobal<&ui::HistoryActions::undo, &ui::HistoryActions::canUndo>(ru, record_actions);
        auto bm = commands.bindGlobal<&ui::HistoryMenuActions::undo, &ui::HistoryMenuActions::canUndo>(mu, menu);
        assert(bt && br && bb && bm);
        assert(histories->activate(b->handle()));
        assert(commands.invoke(tu) == lux::ui::ECommandDispatchResult::EXECUTED);
        assert(text.text() == "alpha" && record.records().at(1) == 4);
        assert(!second_text_pane.canUndo() && second_text_pane.canRedo());
        const auto record_before = Snapshot(record);
        assert(commands.invoke(tu) == lux::ui::ECommandDispatchResult::DISABLED);
        assert(record_before == Snapshot(record)); // No fallback from an empty local stack.
        assert(commands.invoke(tr) == lux::ui::ECommandDispatchResult::EXECUTED);
        assert(histories->activate(a->handle()) && menu.capture());
        assert(histories->activate(b->handle()));
        assert(menu.capturedTarget() == a->handle());
        assert(commands.invoke(mu) == lux::ui::ECommandDispatchResult::EXECUTED);
        assert(text.text() == "alpha" && record_before == Snapshot(record));
        assert(*histories->activeTarget() == b->handle());
        assert(commands.invoke(tr) == lux::ui::ECommandDispatchResult::EXECUTED);
        text.blocked = true;
        assert(commands.invoke(tu) == lux::ui::ECommandDispatchResult::DISABLED && direct.calls == 0);
        text.blocked = false;
        text.reject_prepare = true;
        const Snapshot failed_before(text);
        assert(commands.invoke(tu) == lux::ui::ECommandDispatchResult::EXECUTED);
        assert(failed_before == Snapshot(text) && direct.calls == 1);
        assert(direct.last.target == text.historyId() && direct.last.failure.code == EEditError::PRECONDITION_FAILED);
        assert(messages.dispatchPending() == 1 && queued.calls == 1);
        assert(queued.last.target == text.historyId() && queued.last.failure.code == EEditError::PRECONDITION_FAILED);
        text.reject_prepare = false;
        text.preview = true;
        const Snapshot preview_before(text);
        assert(commands.invoke(tu) == lux::ui::ECommandDispatchResult::EXECUTED);
        assert(!text.preview && preview_before == Snapshot(text));
        assert(a->reset());
        assert(!menu.canUndo());
        menu.undo();
        assert(direct.calls == 2 && direct.last.failure.code == EEditError::STALE_TARGET);
        assert(record_before == Snapshot(record));
        std::thread foreign(
            [&]
            {
                assert(!menu.capture() && !menu.canUndo() && !text_actions.canUndo());
                text_actions.undo();
                menu.undo();
            });
        foreign.join();
        assert(direct.calls == 2 && failed_before == Snapshot(text));
        assert(histories->deactivate() && menu.capture() && !menu.capturedTarget().valid());
        assert(!menu.canUndo() && !menu.canRedo());
        assert(b->reset() && histories->close());
    }
    lux::meta::ReflectionRegistry::destroyRegistry();
    std::puts(
        "ER1 history actions: fixed targets, no fallback, captured menu token, signals, preview cancellation passed");
}

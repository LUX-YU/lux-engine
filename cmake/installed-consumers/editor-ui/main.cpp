#include <lux/engine/editor/ui/actions/HistoryActions.hpp>
#include <lux/engine/editor/ui/shell/EditorWindow.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/ui/CommandRouter.hpp>
#include <cassert>
#include <cstdio>
#include <type_traits>
class Sink final : public lux::object::Object<Sink>
{
public:
    using Object::Object;
    unsigned calls{};
    lux::editor::ui::HistoryActionFailure last;
    void receive(const lux::editor::ui::HistoryActionFailure &value) noexcept
    {
        ++calls;
        last = value;
    }
};
int main()
{
    namespace ui = lux::editor::ui;
    static_assert(!std::is_move_constructible_v<ui::EditorWindow>);
    static_assert(!std::is_copy_constructible_v<ui::EditorWindow>);
    lux::meta::ReflectionRegistry::initRegistry();
    {
        lux::object::ObjectMessageQueue messages;
        auto router = ui::ActiveEditHistory::create(4);
        assert(router);
        ui::HistoryMenuActions actions(messages.dispatcherRef(), **router);
        Sink direct(messages.dispatcherRef()), queued(messages.dispatcherRef());
        auto connection = actions.observe<ui::HistoryMenuActions::failed, &Sink::receive,
            lux::object::EDelivery::DIRECT>(direct);
        auto deferred = actions.observe<ui::HistoryMenuActions::failed, &Sink::receive,
            lux::object::EDelivery::QUEUED>(queued);
        assert(connection && deferred);
        lux::ui::CommandRouter commands;
        const auto command = commands.defineCommand({lux::ui::UiCommandId{"installed.menu.undo"}, "Undo"});
        assert(command);
        auto binding = commands.bindGlobal<&ui::HistoryMenuActions::undo>(*command, actions);
        assert(binding);
        assert(commands.invoke(*command) == lux::ui::ECommandDispatchResult::EXECUTED && direct.calls == 1);
        assert(direct.last.failure.code == lux::editor::editing::EEditError::STALE_TARGET);
        assert(messages.dispatchPending() == 1 && queued.calls == 1);
        assert(queued.last.failure.code == direct.last.failure.code);
        assert((*router)->close());
        std::puts("Generic installed Editor UI: actual CommandRouter and generated DIRECT/QUEUED signal passed");
    }
    lux::meta::ReflectionRegistry::destroyRegistry();
}

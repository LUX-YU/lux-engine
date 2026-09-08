#include <cassert>
#include <iostream>
#include <lux/engine/editor/EditHistoryController.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/ui/CommandRouter.hpp>
class Sink final : public lux::object::Object<Sink>
{
public:
    using Object::Object;
    unsigned calls{};
    void receive(const lux::editor::EditHistoryActionFailure& error) noexcept
    {
        assert(error.failure.code == lux::editor::editing::EEditError::NO_ACTIVE_TARGET);
        ++calls;
    }
};
int main()
{
    lux::object::ObjectMessageQueue queue;
    auto histories = lux::editor::ActiveEditHistory::create(2U);
    assert(histories);
    lux::editor::EditHistoryController controller(queue.dispatcherRef(), **histories);
    Sink sink(queue.dispatcherRef());
    auto observation =
        controller.observe<lux::editor::EditHistoryController::failed, &Sink::receive, lux::object::EDelivery::DIRECT>(
            sink
        );
    assert(observation);
    lux::ui::CommandRouter router;
    const auto command = router.defineCommand({lux::ui::UiCommandId{"edit.undo"}, "Undo"});
    assert(command);
    auto binding = router.bindGlobal<&lux::editor::EditHistoryController::undo>(*command, controller);
    assert(binding);
    assert(router.invoke(*command) == lux::ui::ECommandDispatchResult::EXECUTED && sink.calls == 1U);
    std::cout << "META_INSTALL PASS installed Controller signal and CommandRouter\n";
}

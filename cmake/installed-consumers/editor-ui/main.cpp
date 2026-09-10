#include <lux/engine/editor/ui/actions/HistoryActions.hpp>
#include <lux/engine/editor/ui/shell/EditorWindow.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/ui/CommandRouter.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
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
class FontPane final : public lux::object::Object<FontPane, lux::ui::Pane>
{
public:
    explicit FontPane(lux::ui::UISession &session)
        : Object(session.dispatcherRef(), lux::ui::PaneId{"installed.font"},
                 lux::ui::PaneTypeId{"installed.font"}, "Text") {}
    std::string text;
private:
    void draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &) override
    {
        static_cast<void>(frame.inputText("Filter", text));
    }
};
int main(int argc, char **argv)
{
    namespace ui = lux::editor::ui;
    static_assert(!std::is_move_constructible_v<ui::EditorWindow>);
    static_assert(!std::is_copy_constructible_v<ui::EditorWindow>);
    lux::meta::ReflectionRegistry::initRegistry();
    {
        lux::window::GlfwRuntime platform;
        assert(platform.valid());
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
        ui::WindowSpec spec;
        spec.visible = false;
        if (argc == 2)
        {
            spec.font.emplace();
            spec.font->file = std::string(argv[1]) + ".missing";
            const auto missing = ui::EditorWindow::create(messages.dispatcherRef(), spec);
            assert(!missing && missing.error().code == ui::EWindowError::FONT_OPEN_FAILURE);
            spec.font->file = argv[1];
        }
        auto window = ui::EditorWindow::create(messages.dispatcherRef(), spec);
        assert(window);
        {
            auto &session = (*window)->uiSession();
            FontPane pane(session);
            auto registration = session.registerPane(pane);
            assert(registration);
            session.setSplitLayout({"installed.font", "", "", "", 260, 0, 0});
            const auto draw = [&]
            {
                assert((*window)->beginFrame({{800, 600}, 1.0F / 60, {1, 1}}));
                assert((*window)->drawPanes());
                assert((*window)->finishFrame());
            };
            draw();
            session.feedInput(lux::ui::UiWindowFocus{true});
            assert(session.requestFocus(pane.id().view()));
            draw();
            session.feedInput(lux::ui::UiPointerMove{{50, 40}});
            session.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, true});
            draw();
            session.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, false});
            draw();
            assert(session.inputSnapshot().keyboard_blocked);
            session.feedInput(lux::ui::UiText{U'你'});
            draw();
            assert(pane.text == "\xE4\xBD\xA0");
            assert(session.textInputAnchor().valid);
            const auto platform = (*window)->textInputPlatformStatus();
            assert(platform && platform->state == ui::ETextInputPlatformState::INACTIVE); // Hidden native target.
            session.feedInput(lux::ui::UiWindowFocus{false});
            assert(!session.textInputAnchor().valid);
        }
        assert((*window)->closeAfterRendererStopped());
        std::printf("Installed cold UI PASS external_font=%d exact_utf8=1 hidden_native_anchor_inactive=1\n", argc == 2);
    }
    lux::meta::ReflectionRegistry::destroyRegistry();
}

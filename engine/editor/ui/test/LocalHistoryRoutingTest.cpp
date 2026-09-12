#include "EditingFixtures.hpp"
#include <lux/engine/editor/ui/actions/HistoryActions.hpp>
#include <lux/engine/editor/ui/shell/EditorWindow.hpp>
#include <lux/engine/editor/ui/detail/WindowTestAccess.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <cstdio>

namespace
{
    using namespace lux::editor;
    using namespace editing::test;
    class TextPane final : public lux::object::Object<TextPane, lux::ui::Pane>
    {
    public:
        TextPane(lux::object::ObjectDispatcherRef dispatcher, TextSession &session)
            : Object(dispatcher, lux::ui::PaneId{"test.text"}, lux::ui::PaneTypeId{"test.text"}, "Text"),
              session_(session)
        {
        }

    private:
        TextSession &session_;
        std::string filter_;
        void draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &context) override
        {
            context.activateContext(lux::ui::UiContextIdView{id().name()});
            static_cast<void>(frame.inputText("Filter", filter_));
            frame.text(session_.text());
        }
    };
    class RecordPane final : public lux::object::Object<RecordPane, lux::ui::Pane>
    {
    public:
        RecordPane(lux::object::ObjectDispatcherRef dispatcher, RecordSession &session)
            : Object(dispatcher, lux::ui::PaneId{"test.record"}, lux::ui::PaneTypeId{"test.record"}, "Records"),
              session_(session)
        {
        }

    private:
        RecordSession &session_;
        void draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &context) override
        {
            context.activateContext(lux::ui::UiContextIdView{id().name()});
            frame.text(session_.records().empty() ? "No records" : "Record document");
        }
    };
    void draw(ui::EditorWindow &window)
    {
        assert(window.beginFrame({{800, 600}, 1.0F / 60, {1, 1}}));
        assert(window.drawPanes());
        assert(window.finishFrame());
    }
    void focus(ui::EditorWindow &window, lux::ui::Pane &pane)
    {
        // Materialize all windows before asking ImGui to focus an existing one.
        draw(window);
        assert(window.uiSession().requestFocus(pane.id().view()));
        for (unsigned i = 0; i < 3; ++i)
            draw(window);
        auto *focused = window.uiSession().focusedPane();
        if (focused != &pane)
            std::fprintf(stderr, "focus expected=%s actual=%s\n", pane.id().name().data(),
                         focused ? focused->id().name().data() : "none");
        assert(focused == &pane);
    }
    void chord(ui::EditorWindow &window, lux::ui::EKey key)
    {
        auto &input = window.uiSession();
        input.feedInput(lux::ui::UiKey{lux::ui::EKey::LEFT_CONTROL, true});
        input.feedInput(lux::ui::UiKey{key, true});
        draw(window);
        input.feedInput(lux::ui::UiKey{key, false});
        input.feedInput(lux::ui::UiKey{lux::ui::EKey::LEFT_CONTROL, false});
        draw(window);
    }
} // namespace
int main()
{
    lux::meta::ReflectionRegistry::initRegistry();
    {
        lux::window::GlfwRuntime platform;
        assert(platform.valid());
        lux::object::ObjectMessageQueue messages;
        ui::WindowSpec spec;
        spec.visible = false;

        auto made = ui::EditorWindow::create(messages.dispatcherRef(), spec);
        assert(made);
        auto &window = **made;
        auto &uis = window.uiSession();
        TextSession text;
        RecordSession record;
        execute(text, text.replace(0, text.text(), "changed"));
        execute(record, record.patch(1, {}, 4));
        TextPane a(uis.dispatcherRef(), text);
        RecordPane b(uis.dispatcherRef(), record);
        auto pa = uis.registerPane(a);
        auto pb = uis.registerPane(b);
        assert(pa && pb);
        uis.setSplitLayout({"test.text", "test.record", "", "", 260, 0, 0});
        ui::HistoryActions text_actions(uis.dispatcherRef(), text), record_actions(uis.dispatcherRef(), record);
        auto &commands = uis.commandRouter();
        auto undo = *commands.findCommand(lux::ui::UiCommandIdView{"lux.edit.undo"});
        auto redo = *commands.findCommand(lux::ui::UiCommandIdView{"lux.edit.redo"});
        auto au = commands.bind<&ui::HistoryActions::undo, &ui::HistoryActions::canUndo>(
            undo, lux::ui::UiContextId{a.id().name()}, a, text_actions);
        auto ar = commands.bind<&ui::HistoryActions::redo, &ui::HistoryActions::canRedo>(
            redo, lux::ui::UiContextId{a.id().name()}, a, text_actions);
        auto bu = commands.bind<&ui::HistoryActions::undo, &ui::HistoryActions::canUndo>(
            undo, lux::ui::UiContextId{b.id().name()}, b, record_actions);
        auto global =
            commands.bindGlobal<&ui::HistoryActions::undo, &ui::HistoryActions::canUndo>(undo, record_actions);
        assert(au && ar && bu && global);
        auto active = window.activeHistory().registerTarget(record);
        assert(active && window.activeHistory().activate(active->handle()));
        uis.feedInput(lux::ui::UiWindowFocus{true});
        focus(window, a);
        const Snapshot record_before(record);
        chord(window, lux::ui::EKey::Z);
        assert(text.text() == "alpha" && record_before == Snapshot(record));
        chord(window, lux::ui::EKey::Z);
        assert(text.text() == "alpha" && record_before == Snapshot(record));
        chord(window, lux::ui::EKey::Y);
        assert(text.text() == "changed" && record_before == Snapshot(record));

        // Real ImGui InputText gets focus through ordinary UISession pointer events.
        // Its empty local text undo stack must still intercept Ctrl+Z.
        uis.feedInput(lux::ui::UiPointerMove{{50, 35}});
        uis.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, true});
        draw(window);
        uis.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, false});
        draw(window);
        assert(uis.inputSnapshot().keyboard_blocked);
        const Snapshot text_before(text);
        chord(window, lux::ui::EKey::Z);
        assert(text_before == Snapshot(text) && record_before == Snapshot(record));

        focus(window, b);
        chord(window, lux::ui::EKey::Z);
        assert(record.records().empty() && text_before == Snapshot(text));
        assert(active->reset());
        au->reset();
        ar->reset();
        bu->reset();
        global->reset();
        pa->reset();
        pb->reset();
        assert(window.closeAfterRendererStopped());
        std::puts(
            "Local history: actual Pane focus, fixed Text/Record targets, empty-stack and InputText isolation passed");
    }
    lux::meta::ReflectionRegistry::destroyRegistry();
}

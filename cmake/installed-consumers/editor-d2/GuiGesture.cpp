#include <InspectorWidget.hpp>
#include <cassert>
#include <consumer/Component.hpp>
#include <consumer/Gui.hpp>
#include <cstdio>
#include <lux/engine/editor/gui/GuiView.hpp>

namespace consumer
{
    namespace
    {
        struct OmissionProbe final
        {
            lux::editor::gui::InspectorInteraction *interaction{};
            ImGuiContext *context{};
            ImVec2 center{};
            std::size_t draws{};
            std::shared_ptr<bool> reject_commit{std::make_shared<bool>(false)};
        };
        OmissionProbe *omission{};
    } // namespace

    bool drawOmissionProbe(lux::editor::scene::SceneEditor &document, lux::world::WorldObjectId object,
                           lux::ui::Frame &frame, lux::editor::gui::InspectorInteraction &interaction)
    {
        if (!omission)
        {
            return false;
        }
        auto &probe = *omission;
        probe.interaction = &interaction;
        probe.context = ImGui::GetCurrentContext();
        ImGui::GetIO().MouseDoubleClickTime = 0;
        ++probe.draws;
        interaction.field<Component, double>(
            document, object, frame, "omission.gain", "Omission gain",
            [reject = probe.reject_commit](auto &value) noexcept
            {
                using Pointer = decltype(&value.settings.gain);
                return *reject ? Pointer{} : &value.settings.gain;
            },
            [&probe](double &value, auto &)
            {
                const bool changed = ImGui::DragScalar("##value", ImGuiDataType_Double, &value, 0.1F);
                const auto low = ImGui::GetItemRectMin();
                const auto high = ImGui::GetItemRectMax();
                probe.center = {(low.x + high.x) / 2, (low.y + high.y) / 2};
                return lux::editor::gui::generated_support::edited(changed);
            },
            false);
        return true;
    }

    void checkUndrawnInspector(lux::editor::scene::SceneEditor &document, lux::editor::gui::EditorWindow &window,
                               const std::function<void()> &restore)
    {
        using namespace lux::editor;
        auto &ui = window.uiSession();
        gui::GuiView *inspector{};
        for (const auto &view : document.views())
        {
            if (view->id().ends_with("-inspector"))
            {
                inspector = dynamic_cast<gui::GuiView *>(view.get());
            }
        }
        assert(inspector);
        auto &pane = inspector->pane();
        auto &view = *dynamic_cast<DocumentView *>(inspector);
        OmissionProbe probe;
        omission = &probe;
        const auto before = document.historyView()->history;
        const auto object = document.selection().object;
        const auto read = [&]()
        {
            return static_cast<const Component *>(document.component(object, lux::cxx::typeToken<Component>()))
                ->settings.gain;
        };
        const auto original = read();
        const std::string label = std::string(pane.title()) + "###" + std::string(pane.id().name());
        const auto draw = [&](float width, bool collapse = false)
        {
            assert(window.beginFrame({{width, 700}, 1.0F / 60}));
            if (probe.context)
            {
                auto *previous = ImGui::GetCurrentContext();
                ImGui::SetCurrentContext(probe.context);
                ImGui::SetWindowCollapsed(label.c_str(), collapse);
                ImGui::SetCurrentContext(previous);
            }
            assert(window.drawPanes());
            assert(window.finishFrame());
        };
        const auto poll = [&]()
        {
            PollBudget budget;
            view.poll(budget);
        };
        const auto begin = [&]()
        {
            assert(ui.requestFocus(pane.id().view()));
            draw(1000);
            ui.feedInput(lux::ui::UiPointerMove{{probe.center.x, probe.center.y}});
            draw(1000);
            ui.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, true});
            draw(1000);
            ui.feedInput(lux::ui::UiPointerMove{{probe.center.x + 40, probe.center.y}});
            draw(1000);
            assert(pane.focused() && probe.interaction->active() && read() != original);
        };
        begin();
        const auto drawn = probe.draws;
        for (unsigned iteration = 0; iteration != 8; ++iteration)
        {
            poll(); // No new UI frame, as while a prepared Renderer packet is backpressured.
        }
        assert(probe.interaction->active() && probe.draws == drawn);
        assert(document.historyView()->history.current == before.current);
        bool focus_notification{};
        auto connection = pane.observeScoped<lux::ui::Pane::focusChanged>(
            [&](const lux::ui::PaneFocusChanged &notice) noexcept
            {
                if (!notice.focused)
                {
                    focus_notification = true;
                    assert(probe.interaction->active()); // Notification only changes UI facts.
                    assert(document.historyView()->history.current == before.current);
                }
            });
        draw(800); // Split layout gives the right pane zero width without changing Pane.visible.
        assert(pane.visible() && !pane.focused() && probe.draws == drawn && focus_notification);
        connection.reset();
        poll();
        std::printf("Undrawn Inspector: visible=%d focused=%d draws=%zu active=%d cursor=%zu\n", pane.visible(),
                    pane.focused(), probe.draws, probe.interaction->active(), document.historyView()->history.cursor);
        std::fflush(stdout);
        assert(!probe.interaction->active() && document.historyView()->history.cursor == before.cursor + 1);
        ui.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, false});
        draw(1000);
        poll();
        assert(document.historyView()->history.cursor == before.cursor + 1);
        assert(document.undo() && read() == original);

        begin();
        const auto second_drawn = probe.draws;
        draw(1000, true);
        assert(pane.visible() && !pane.focused() && probe.draws == second_drawn);
        *probe.reject_commit = true;
        poll();
        assert(probe.interaction->active() && document.historyView()->history.current == before.current);
        assert(probe.interaction->failure().code == editing::EEditError::PRECONDITION_FAILED);
        *probe.reject_commit = false;
        poll();
        assert(!probe.interaction->active() && document.historyView()->history.cursor == before.cursor + 1);
        ui.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, false});
        draw(1000);
        poll();
        assert(document.historyView()->history.cursor == before.cursor + 1);
        assert(document.undo() && read() == original);
        begin();
        const auto cancelled = document.historyView()->history;
        ui.feedInput(lux::ui::UiKey{lux::ui::EKey::ESCAPE, true});
        draw(1000);
        poll();
        assert(!probe.interaction->active() && read() == original);
        assert(document.historyView()->history.current == cancelled.current);
        assert(document.historyView()->history.revision == cancelled.revision);
        ui.feedInput(lux::ui::UiKey{lux::ui::EKey::ESCAPE, false});
        draw(1000);
        ui.feedInput(lux::ui::UiPointerMove{{probe.center.x + 80, probe.center.y}});
        draw(1000);
        ui.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, false});
        draw(1000);
        assert(!probe.interaction->active() && read() == original);
        assert(document.historyView()->history.revision == cancelled.revision);
        const auto pane_id = std::string(view.id());
        const auto views = document.views().size();
        begin();
        view.requestClose();
        PollBudget budget;
        document.poll(budget);
        assert(document.views().size() + 1 == views && read() == original);
        assert(document.historyView()->history.current == before.current);
        probe.interaction = nullptr;
        ui.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, false});

        unsigned retired_notices{}, live_notices{};
        auto retired = document.observeScoped<scene::SceneEditor::selectionChanged>(
            [&](const scene::SelectionNotice &) noexcept { ++retired_notices; });
        auto live = document.observeScoped<scene::SceneEditor::selectionChanged>(
            [&](const scene::SelectionNotice &) noexcept { ++live_notices; });
        retired.reset();
        assert(document.select({}) && document.select(object));
        assert(retired_notices == 0 && live_notices == 2);
        restore();
        assert(document.views().size() == views);
        assert(ui.requestFocus(lux::ui::PaneIdView{pane_id}));
        const auto before_rebuild_draw = probe.draws;
        draw(1000);
        assert(probe.interaction && !probe.interaction->active() && probe.draws == before_rebuild_draw + 1);
        assert(document.historyView()->history.current == before.current);
        ui.feedInput(lux::ui::UiPointerMove{{probe.center.x, probe.center.y}});
        draw(1000);
        ui.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, true});
        draw(1000);
        ui.feedInput(lux::ui::UiPointerMove{{probe.center.x + 40, probe.center.y}});
        draw(1000);
        ui.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, false});
        draw(1000);
        document.poll(budget);
        assert(document.historyView()->history.cursor == before.cursor + 1);
        assert(document.undo() && read() == original);
        assert(retired_notices == 0 && live_notices == 2);
        // History owns the accessor's shared rejection flag after this probe ends.
        assert(document.historyView()->history.current == before.current);
        omission = nullptr;
        std::puts(
            "PASS real Inspector omission: layout zero/collapse retain visible; focus notifications do not edit; "
            "owner poll commits; rejected commit retries same token; no-draw turns retain active gesture; Undo "
            "restores; "
            "injected Esc restores without restarting while mouse remains held; close cancels preview; "
            "selection while destroyed, rebuilt Inspector refresh and edit/Undo pass with isolated subscriptions");
    }

    void checkClippedGesture(lux::editor::scene::SceneEditor &document, lux::world::WorldObjectId object,
                             lux::ui::Frame &frame)
    {
        using namespace lux::editor;
        // A separate ImGui context injects IO events; this is not physical desktop evidence.
        struct Context final
        {
            ImGuiContext *previous{ImGui::GetCurrentContext()};
            ImGuiContext *current{ImGui::CreateContext()};
            ~Context()
            {
                ImGui::DestroyContext(current);
                ImGui::SetCurrentContext(previous);
            }
        } context;
        ImGui::SetCurrentContext(context.current);
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {640, 480};
        io.DeltaTime = 1.0F / 60;
        io.MouseDoubleClickTime = 0; // Each scripted press is a separate drag, not a text-entry double click.
        unsigned char *pixels{};
        int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        gui::InspectorInteraction interaction(document, "clipped-gesture");
        const auto before = document.historyView()->history;
        const auto access = [](auto &value) noexcept { return &value.settings.gain; };
        const auto read = [&]()
        {
            return static_cast<const Component *>(document.component(object, lux::cxx::typeToken<Component>()))
                ->settings.gain;
        };
        const auto original = read();
        ImVec2 center{};
        const auto draw = [&](bool visible)
        {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({600, 400});
            ImGui::Begin("Gesture regression");
            {
                auto table = frame.table({lux::ui::WidgetIdView{"fields"}, 2});
                assert(table.visible());
                if (visible)
                {
                    interaction.field<Component, double>(
                        document, object, frame, "settings.gain", "Gain", access,
                        [&](double &value, auto &)
                        {
                            const bool changed = ImGui::DragScalar("##value", ImGuiDataType_Double, &value, 0.1F);
                            const auto minimum = ImGui::GetItemRectMin();
                            const auto maximum = ImGui::GetItemRectMax();
                            center = {(minimum.x + maximum.x) / 2, (minimum.y + maximum.y) / 2};
                            return gui::generated_support::edited(changed);
                        },
                        false);
                }
            }
            assert(interaction.finishDraw());
            ImGui::End();
            ImGui::Render();
        };
        draw(true);
        draw(true);
        io.AddMousePosEvent(center.x, center.y);
        draw(true);
        io.AddMouseButtonEvent(0, true);
        draw(true);
        assert(interaction.active());
        io.AddMousePosEvent(center.x + 40, center.y);
        draw(true);
        assert(read() != original && document.historyView()->history.current == before.current);
        // Pagination/clipping omits the active field while release arrives through the same IO system.
        io.AddMouseButtonEvent(0, false);
        draw(false);
        draw(false);
        draw(false);
        std::printf("Clipped gesture: active=%d cursor=%zu before=%zu original=%.3f value=%.3f\n", interaction.active(),
                    document.historyView()->history.cursor, before.cursor, original, read());
        std::fflush(stdout);
        assert(!interaction.active());
        assert(document.historyView()->history.cursor == before.cursor + 1);
        draw(true);
        assert(document.historyView()->history.cursor == before.cursor + 1);
        assert(document.undo() && read() == original);
        assert(document.historyView()->history.current == before.current);

        const auto no_change = document.historyView()->history;
        io.AddMousePosEvent(center.x, center.y);
        draw(true);
        io.AddMouseButtonEvent(0, true);
        draw(true);
        assert(interaction.active());
        io.AddMouseButtonEvent(0, false);
        draw(true);
        assert(!interaction.active() && read() == original);
        assert(document.historyView()->history.current == no_change.current);
        assert(document.historyView()->history.revision == no_change.revision);

        io.AddMouseButtonEvent(0, true);
        draw(true);
        io.AddMousePosEvent(center.x + 60, center.y);
        draw(true);
        assert(interaction.active() && read() != original);
        draw(false);
        assert(!interaction.active() && document.historyView()->history.cursor == before.cursor + 1);
        // Reappear while still held: the old widget must not restart its retired preview.
        draw(true);
        io.AddMouseButtonEvent(0, false);
        draw(true);
        draw(true);
        assert(!interaction.active() && document.historyView()->history.cursor == before.cursor + 1);
        assert(document.undo() && read() == original);
        std::puts("PASS injected ImGui gesture: omitted control commits once while held or released; redraw does not "
                  "restart; unchanged click preserves revision; Undo restores");
    }
} // namespace consumer

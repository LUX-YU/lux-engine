#include <InspectorWidget.hpp>
#include <cassert>
#include <consumer/Component.hpp>
#include <consumer/Domain.hpp>
#include <consumer/Gui.hpp>
#include <cstdio>
#include <imgui_internal.h>
#include <lux/engine/editor/gui/GuiView.hpp>
#include <lux/engine/editor/gui/scene/SceneDocumentProvider.hpp>
#include <lux/engine/editor/ui/UIRenderSystem.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <thread>

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

bool drawOmissionProbe(lux::editor::scene::SceneEditor &document, lux::editor::scene::SceneEntityRef object,
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
        [reject = probe.reject_commit](auto &value) noexcept {
            using Pointer = decltype(&value.settings.gain);
            return *reject ? Pointer{} : &value.settings.gain;
        },
        [&probe](double &value, auto &state) {
            const auto original = value;
            const bool changed = ImGui::DragScalar("##value", ImGuiDataType_Double, &value, 0.1F);
            const auto low = ImGui::GetItemRectMin();
            const auto high = ImGui::GetItemRectMax();
            probe.center = {(low.x + high.x) / 2, (low.y + high.y) / 2};
            return lux::editor::gui::generated_support::edited(state.changed(value, original, changed));
        },
        false);
    return true;
}

void checkUndrawnInspector(lux::editor::scene::SceneEditor &document, lux::render::RenderRuntime &runtime,
                           lux::process::ExecutionRuntime &process, const std::function<void()> &restore_root)
{
    using namespace lux::editor;
    const auto close_views = [&] {
        for (const auto &view : document.views())
        {
            view->requestClose();
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!document.views().empty())
        {
            assert(std::chrono::steady_clock::now() < deadline);
            PollBudget budget;
            document.poll(budget);
            std::size_t controls = 8, programs = 4;
            assert(runtime.poll(32, controls, programs));
        }
    };
    close_views();
    ui::UIRenderSystemConfig config;
    const auto registration = ui::uiRenderSystemRegistration();
    const lux::simulation::ecs::ComponentSchemaSet task_components{};
    const lux::simulation::SimulationSystemRegistry task_system_types;
    const std::array task_scene_systems{registration};
    lux::scene::SceneDescriptionBuilder builder;
    assert(builder.addSystem({1}, "ui", registration.type, 1, {}, 0));
    auto description = std::move(builder).buildResolved();
    assert(description);
    auto dispatcher = document.dispatcherRef();
    std::array providers{
        lux::scene::makeSceneCapabilityProvider<lux::render::RenderRuntime>("runtime", "lux.render.runtime", runtime),
        lux::scene::makeSceneCapabilityProvider<lux::object::ObjectDispatcherRef>("dispatcher", "lux.object.dispatcher",
                                                                                  dispatcher),
        lux::scene::makeSceneCapabilityProvider<ui::UIRenderSystemConfig>("config", "lux.editor.ui.config", config)};
    auto created = lux::scene::SceneInstance::create(
        {std::make_shared<const lux::scene::SceneDescription>(std::move(*description)),
         std::make_shared<const lux::world::WorldDescription>(),
         std::make_shared<const lux::simulation::SimulationDescription>(), task_components, task_system_types, task_scene_systems, providers,
         lux::simulation::ESimulationMode::DERIVATION});
    assert(created && (*created)->simulation().seal());
    auto instance = std::move(*created);
    auto &ui = *instance->findSceneSystem<ui::UIRenderSystem>();
    const auto receipt = ui.resourceReceipt();
    // This UI-only host supplies the layout's toolbar slot. Project actions
    // themselves are exercised by the enclosing formal Editor workflow.
    struct Toolbar final : lux::object::Object<Toolbar, lux::ui::Pane>
    {
        explicit Toolbar(lux::object::ObjectDispatcherRef dispatcher)
            : Object(dispatcher, lux::ui::PaneId{"project"}, lux::ui::PaneTypeId{"test.toolbar"}, "Project")
        {
        }
        void draw(lux::ui::Frame &, lux::ui::PaneDrawContext &) override
        {
        }
    } toolbar(dispatcher);
    auto toolbar_registration = ui.registerPane(toolbar);
    assert(toolbar_registration);
    assert(ui.commandRouter().defineCommand({lux::ui::UiCommandId{"lux.edit.undo"}, "Undo"}));
    assert(ui.commandRouter().defineCommand({lux::ui::UiCommandId{"lux.edit.redo"}, "Redo"}));
    auto executor = lux::task::TaskExecutor::create({0, 1024});
    assert(executor);
    lux::scene::SceneDriver driver(*executor);
    const auto restore = [&] {
        const std::array bindings{binding()};
        auto provider = gui::sceneDocumentProvider(schemas(), bindings);
        const auto attached = provider.attach(document, ui, runtime, process);
        if (!attached)
        {
            std::fprintf(stderr, "Pane attach: %s:%llu %s\n", attached.error().domain.c_str(), attached.error().reason,
                         attached.error().message.c_str());
        }
        assert(attached);
    };
    restore();
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
    const auto read = [&]() {
        return static_cast<const Component *>(document.component(object, lux::cxx::typeToken<Component>()))
            ->settings.gain;
    };
    const auto original = read();
    const std::string label = std::string(pane.title()) + "###" + std::string(pane.id().name());
    const auto draw = [&](float width, bool collapse = false) {
        auto frame = ui.beginFrame({{width, 700}, 1.0F / 60});
        if (probe.context)
        {
            auto *previous = ImGui::GetCurrentContext();
            ImGui::SetCurrentContext(probe.context);
            if (collapse)
            {
                // A docked tab cannot collapse independently. Float this actual Pane
                // first, as a user can, then exercise its no-draw lifecycle.
                ImGui::DockBuilderDockWindow(label.c_str(), 0);
            }
            ImGui::SetWindowCollapsed(label.c_str(), collapse);
            ImGui::SetCurrentContext(previous);
        }
        ui.drawPanes(frame);
        assert(ui.finishFrame(frame));
        driver.invalidate(*instance);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        do
        {
            assert(std::chrono::steady_clock::now() < deadline);
            std::size_t controls = 8, programs = 4;
            assert(runtime.poll(32, controls, programs));
            lux::scene::SceneAdvanceBudget advance{32, 1, 1};
            static_cast<void>(driver.advance(*instance, std::chrono::steady_clock::now(), advance));
            assert(instance->progress().result);
            if (!ui.canBuildFrame())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        } while (!ui.canBuildFrame());
    };
    const auto poll = [&]() {
        PollBudget budget;
        view.poll(budget);
    };
    const auto begin = [&]() {
        assert(ui.requestFocus(pane.id().view()));
        draw(1000);
        ui.feedInput(lux::ui::UiPointerMove{{probe.center.x, probe.center.y}});
        draw(1000);
        ui.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, true});
        draw(1000);
        ui.feedInput(lux::ui::UiPointerMove{{probe.center.x + 40, probe.center.y}});
        draw(1000);
        std::printf("Inspector drag: focused=%d active=%d draws=%zu center=(%.1f,%.1f) value=%.3f original=%.3f\n",
                    pane.focused(), probe.interaction->active(), probe.draws, probe.center.x, probe.center.y, read(),
                    original);
        std::fflush(stdout);
        assert(pane.focused() && probe.interaction->active() && read() != original);
    };
    // Allow the cold DockBuilder layout to settle before targeting screen coordinates.
    for (unsigned warmup{}; warmup != 4; ++warmup)
    {
        draw(1000);
    }
    begin();
    const auto drawn = probe.draws;
    for (unsigned iteration = 0; iteration != 8; ++iteration)
    {
        poll(); // No new UI frame, as while a prepared Renderer packet is backpressured.
    }
    assert(probe.interaction->active() && probe.draws == drawn);
    assert(document.historyView()->history.current == before.current);
    bool focus_notification{};
    auto connection =
        pane.observeScoped<lux::ui::Pane::focusChanged>([&](const lux::ui::PaneFocusChanged &notice) noexcept {
            if (!notice.focused)
            {
                focus_notification = true;
                assert(probe.interaction->active()); // Notification only changes UI facts.
                assert(document.historyView()->history.current == before.current);
            }
        });
    draw(800); // Docking retains the pane instead of enforcing the former zero-width policy.
    assert(pane.visible() && probe.draws > drawn && probe.interaction->active());
    assert(document.historyView()->history.current == before.current);
    const auto narrow_drawn = probe.draws;
    draw(800, true);
    assert(pane.visible() && !pane.focused() && probe.draws == narrow_drawn && focus_notification);
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
    ui.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, false});
    draw(1000);
    assert(!probe.interaction->active() && read() != original);
    assert(document.historyView()->history.cursor == before.cursor + 1);
    assert(document.undo() && read() == original);
    const auto pane_id = std::string(view.id());
    const auto views = document.views().size();
    begin();
    view.requestClose();
    PollBudget budget;
    document.poll(budget);
    assert(document.views().size() + 1 == views && read() != original);
    assert(document.undo() && read() == original);
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
    close_views();
    toolbar_registration->reset();
    instance.reset();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (receipt.status().state != lux::render::ESceneResourceState::RETIRED)
    {
        assert(std::chrono::steady_clock::now() < deadline);
        std::size_t controls = 8, programs = 4;
        assert(runtime.poll(32, controls, programs));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    restore_root();
    std::puts("PASS real Inspector omission: docking retains narrow panes; floating collapse retains visible; "
              "focus notifications do not edit; "
              "owner poll commits; rejected commit retries same token; no-draw turns retain active gesture; Undo "
              "restores; "
              "ordinary release commits once; close finishes the edit and Undo restores it; "
              "selection while destroyed, rebuilt Inspector refresh and edit/Undo pass with isolated subscriptions");
}

void checkCompletedGesture(lux::editor::scene::SceneEditor &document, lux::editor::scene::SceneEntityRef object,
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
    gui::InspectorInteraction interaction(document, "completed-gesture");
    const auto before = document.historyView()->history;
    const auto access = [](auto &value) noexcept { return &value.settings.gain; };
    const auto read = [&]() {
        return static_cast<const Component *>(document.component(object, lux::cxx::typeToken<Component>()))
            ->settings.gain;
    };
    const auto original = read();
    ImVec2 center{};
    const auto draw = [&](bool visible) {
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
                    [&](double &value, auto &state) {
                        const auto original = value;
                        const bool changed = ImGui::DragScalar("##value", ImGuiDataType_Double, &value, 0.1F);
                        const auto minimum = ImGui::GetItemRectMin();
                        const auto maximum = ImGui::GetItemRectMax();
                        center = {(minimum.x + maximum.x) / 2, (minimum.y + maximum.y) / 2};
                        return gui::generated_support::edited(state.changed(value, original, changed));
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
    assert(!interaction.active());
    io.AddMousePosEvent(center.x + 40, center.y);
    draw(true);
    assert(read() != original && document.historyView()->history.current == before.current);
    // Release ends the edit even if the widget is omitted on that frame.
    io.AddMouseButtonEvent(0, false);
    draw(false);
    draw(false);
    draw(false);
    std::printf("Completed gesture: active=%d cursor=%zu before=%zu original=%.3f value=%.3f\n", interaction.active(),
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
    assert(!interaction.active());
    io.AddMouseButtonEvent(0, false);
    draw(true);
    assert(!interaction.active() && read() == original);
    assert(document.historyView()->history.current == no_change.current);
    assert(document.historyView()->history.revision == no_change.revision);

    std::puts("PASS injected ImGui completed edit: release commits once; omitted widget cannot strand a "
              "completed field edit; redraw does not duplicate history; unchanged click preserves revision; Undo "
              "restores");
}
} // namespace consumer

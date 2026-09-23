#include "../../common/RenderRegistration.hpp"
#include <cassert>
#include <cstdio>
#include <imgui.h>
#include <lux/engine/editor/ui/UIRenderSystem.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/object/detail/MessageEnvelope.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/SceneDriver.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <lux/engine/ui/rendering/UiRenderFeature.hpp>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using UI = lux::editor::ui::UIRenderSystem;
std::unique_ptr<lux::scene::SceneInstance> createUi(lux::render::RenderRuntime &runtime,
                                                    lux::object::ObjectDispatcherRef dispatcher)
{
    using namespace lux;
    editor::ui::UIRenderSystemConfig config;
    const auto registration = editor::ui::uiRenderSystemRegistration();
    const lux::simulation::ecs::ComponentSchemaSet task_components{};
    const lux::simulation::SimulationSystemRegistry task_system_types;
    const std::array task_scene_systems{registration};
    scene::SceneDescriptionBuilder builder;
    assert(builder.addSystem({1}, "ui", registration.type, 1, {}, 0));
    auto description = std::move(builder).buildResolved();
    assert(description);
    std::array providers{
        scene::makeSceneCapabilityProvider<render::RenderRuntime>("runtime", "lux.render.runtime", runtime),
        scene::makeSceneCapabilityProvider<object::ObjectDispatcherRef>("dispatcher", "lux.object.dispatcher",
                                                                        dispatcher),
        scene::makeSceneCapabilityProvider<editor::ui::UIRenderSystemConfig>("config", "lux.editor.ui.config", config)};
    auto created = scene::SceneInstance::create(
        {std::make_shared<const scene::SceneDescription>(std::move(*description)),
         std::make_shared<const world::WorldDescription>(), std::make_shared<const simulation::SimulationDescription>(),
         task_components, task_system_types, task_scene_systems, providers, simulation::ESimulationMode::DERIVATION});
    assert(created && (*created)->simulation().seal());
    return std::move(*created);
}

void drawUi(lux::scene::SceneInstance &instance, lux::render::RenderRuntime &runtime, lux::scene::SceneDriver &driver,
            lux::ui::FrameInfo info)
{
    auto &ui = *instance.findSceneSystem<UI>();
    auto frame = ui.beginFrame(info);
    ui.drawPanes(frame);
    assert(ui.finishFrame(frame));
    driver.invalidate(instance);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    do
    {
        assert(std::chrono::steady_clock::now() < deadline);
        std::size_t controls = 8, programs = 4;
        assert(runtime.poll(32, controls, programs));
        lux::scene::SceneAdvanceBudget budget{32, 1, 1};
        static_cast<void>(driver.advance(instance, std::chrono::steady_clock::now(), budget));
        assert(instance.progress().result);
        if (!ui.canBuildFrame())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } while (!ui.canBuildFrame());
}

class DockProbe final : public lux::ui::Pane
{
  public:
    DockProbe(UI &ui, const char *id)
        : Pane(ui.dispatcherRef(), lux::ui::PaneId{id}, lux::ui::PaneTypeId{"dock-probe"}, id)
    {
    }

    ImGuiID dock{};
    ImVec2 extent;

  private:
    void draw(lux::ui::Frame &, lux::ui::PaneDrawContext &) override
    {
        assert(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_DockingEnable);
        assert(!(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable));
        assert(ImGui::IsWindowDocked());
        dock = ImGui::GetWindowDockID();
        extent = ImGui::GetWindowSize();
        ImGui::TextUnformatted("Dockable content");
    }
};

void checkDocking(lux::render::RenderRuntime &runtime, lux::object::ObjectDispatcherRef dispatcher,
                  lux::scene::SceneDriver &driver)
{
    using namespace lux::ui;
    auto instance = createUi(runtime, dispatcher);
    auto &ui = *instance->findSceneSystem<UI>();
    DockProbe left{ui, "left"}, center{ui, "center"};
    auto left_registration = ui.registerPane(left);
    auto center_registration = ui.registerPane(center);
    ui.setSplitLayout({"left", "center"});
    const auto draw = [&](lux::scene::SceneInstance &scene) {
        drawUi(scene, runtime, driver, {{1200, 800}, 1.0F / 60, {1, 1}});
    };
    draw(*instance);
    draw(*instance);
    assert(left.dock != 0 && center.dock != 0 && left.dock != center.dock);
    const auto left_extent = left.extent;
    const auto left_node = left.dock;
    const auto center_node = center.dock;
    // The host may reapply its default, but must not overwrite live docking.
    auto new_default = SplitLayout{"left", "center"};
    new_default.left_width = 400;
    ui.setSplitLayout(new_default);
    draw(*instance);
    assert(left.extent.x == left_extent.x && left.dock == left_node && center.dock == center_node);
    left.setVisible(false);
    draw(*instance);
    left.setVisible(true);
    draw(*instance);
    draw(*instance);
    assert(left.dock == left_node && center.dock == center_node);
    const auto layout = ui.captureLayout();

    auto other = createUi(runtime, dispatcher);
    auto &restored = *other->findSceneSystem<UI>();
    DockProbe restored_left{restored, "left"}, restored_center{restored, "center"};
    auto a = restored.registerPane(restored_left);
    auto b = restored.registerPane(restored_center);
    assert(restored.restoreLayout(layout));
    restored.setSplitLayout({"left", "center"});
    assert(restored.requestFocus(PaneIdView{"left"}));
    draw(*other);
    draw(*other);
    assert(restored.requestFocus(PaneIdView{"center"}));
    draw(*other);
    draw(*other);
    assert(restored_left.dock == left_node && restored_center.dock == center_node);
    std::puts("PASS UI docking: default splits, hide/restore, no per-frame reset, capture/restore; single native "
              "viewport");
}

class TextProbe final : public lux::ui::Pane
{
  public:
    explicit TextProbe(UI &ui)
        : Pane(ui.dispatcherRef(), lux::ui::PaneId{"text-probe"}, lux::ui::PaneTypeId{"text-probe"}, "Text")
    {
    }

    std::string text;
    bool focus{true}, ctrl{}, shift{}, alt{};

  private:
    void draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &) override
    {
        if (std::exchange(focus, false))
        {
            ImGui::SetKeyboardFocusHere();
        }
        static_cast<void>(frame.inputText("Value", text));
        const auto &io = ImGui::GetIO();
        ctrl = io.KeyCtrl;
        shift = io.KeyShift;
        alt = io.KeyAlt;
    }
};

bool checkTextShortcuts(lux::render::RenderRuntime &runtime, lux::object::ObjectDispatcherRef dispatcher,
                        lux::scene::SceneDriver &driver)
{
    using namespace lux::ui;
    auto instance = createUi(runtime, dispatcher);
    auto &ui = *instance->findSceneSystem<UI>();
    TextProbe pane{ui};
    auto registration = ui.registerPane(pane);
    const auto draw = [&]() { drawUi(*instance, runtime, driver, {{400, 300}, 1.0F / 60, {1, 1}}); };
    const auto key = [&](EKey code, bool down) {
        ui.feedInput(UiKey{code, down});
        draw();
    };
    draw();
    draw();
    ui.feedInput(UiText{U'x'});
    draw();
    assert(pane.text == "x");
    key(EKey::LEFT_CONTROL, true);
    if (!pane.ctrl)
    {
        std::puts("FAIL UI shortcut: physical LeftCtrl is down but ImGui aggregate Ctrl is false; text=x");
        return false;
    }
    key(EKey::RIGHT_CONTROL, true);
    key(EKey::LEFT_CONTROL, false);
    assert(pane.ctrl);
    key(EKey::Z, true);
    assert(pane.text.empty());
    key(EKey::Z, false);
    key(EKey::RIGHT_CONTROL, false);
    assert(!pane.ctrl);
    key(EKey::LEFT_SHIFT, true);
    key(EKey::RIGHT_SHIFT, true);
    key(EKey::LEFT_SHIFT, false);
    assert(pane.shift);
    key(EKey::RIGHT_SHIFT, false);
    assert(!pane.shift);
    key(EKey::LEFT_ALT, true);
    key(EKey::RIGHT_ALT, true);
    key(EKey::LEFT_ALT, false);
    assert(pane.alt);
    ui.feedInput(UiWindowFocus{false});
    draw();
    assert(!pane.ctrl && !pane.shift && !pane.alt);
    ui.feedInput(UiWindowFocus{true});
    draw();
    key(EKey::LEFT_ALT, true);
    key(EKey::LEFT_ALT, false);
    assert(!pane.alt);
    std::puts("PASS UI event-injection: InputText Ctrl+Z; paired Ctrl/Shift/Alt; focus loss clears modifiers");
    return true;
}
} // namespace

int main()
{
    using namespace lux;
    render::RendererConfig config;
    config.validation = true;
    std::vector<lux::render::RenderFeatureRegistration> initial_features = {render::kUiRenderRenderFeatureRegistration};
    auto runtime = render::RenderRuntime::create(std::move(config));
    assert(runtime);
    registerRenderFeatures(**runtime, std::move(initial_features));
    object::ObjectMessageQueue queue;
    const auto dispatcher = queue.dispatcherRef();
    auto executor = task::TaskExecutor::create({0, 1024});
    assert(executor);
    scene::SceneDriver driver(*executor);
    checkDocking(**runtime, dispatcher, driver);
    const bool shortcuts = checkTextShortcuts(**runtime, dispatcher, driver);
    std::vector<int> received;
    const auto post = [&](int value) {
        return object::detail::post(
            dispatcher, object::detail::makeMessage([&, value]() noexcept {
                received.push_back(value);
                if (value == 1)
                {
                    assert(object::detail::post(dispatcher, object::detail::makeMessage([&]() noexcept {
                                                    received.push_back(4);
                                                })) == object::detail::EPostStatus::POSTED);
                }
            }));
    };
    assert(post(1) == object::detail::EPostStatus::POSTED);
    assert(post(2) == object::detail::EPostStatus::POSTED);
    assert(post(3) == object::detail::EPostStatus::POSTED);
    assert(queue.dispatchPending(0) == 0 && received.empty());
    assert(queue.dispatchPending(2) == 2 && received == std::vector<int>({1, 2}));
    assert(queue.dispatchPending(1) == 1 && received.back() == 3);
    assert(queue.dispatchPending(64) == 1 && received.back() == 4);

    auto first = createUi(**runtime, dispatcher);
    auto second = createUi(**runtime, dispatcher);
    assert(post(5) == object::detail::EPostStatus::POSTED);
    {
        drawUi(*first, **runtime, driver, {{400, 300}, 1.0F / 60, {1, 1}});
    }
    assert(received.size() == 4); // Neither UI instance secretly drains the host dispatcher.
    first.reset();
    assert(post(6) == object::detail::EPostStatus::POSTED);
    {
        drawUi(*second, **runtime, driver, {{400, 300}, 1.0F / 60, {1, 1}});
    }
    second.reset();
    assert(queue.dispatchPending(8) == 2 && received.back() == 6);
    queue.close();
    assert(post(7) == object::detail::EPostStatus::CLOSED);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while ((*runtime)->statistics().runtime_leases)
    {
        assert(std::chrono::steady_clock::now() < deadline);
        std::size_t controls = 8, programs = 4;
        assert((*runtime)->poll(32, controls, programs));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert((*runtime)->beginClose());
    for (;;)
    {
        assert(std::chrono::steady_clock::now() < deadline);
        std::size_t replies = 32, controls = 8, programs = 4;
        auto closed = (*runtime)->advanceClose(replies, controls, programs);
        assert(closed);
        if (*closed == render::ERenderClose::COMPLETE)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert((*runtime)->statistics().validation_errors == 0 && (*runtime)->joinStopped());
    if (!shortcuts)
    {
        return 2;
    }
    std::puts("PASS bounded FIFO/reentrant batch/shared host dispatcher/two real UI systems/late closed dispatcher");
}

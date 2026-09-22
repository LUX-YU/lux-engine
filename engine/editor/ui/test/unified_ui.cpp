#include <imgui.h>
#include <lux/engine/editor/ui/UIRenderSystem.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <lux/engine/ui/rendering/UiRenderFeature.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

namespace
{
struct ProbePane final : lux::ui::Pane
{
    std::size_t draws{};
    explicit ProbePane(lux::object::ObjectDispatcherRef dispatcher)
        : Pane(dispatcher, lux::ui::PaneId("probe"), lux::ui::PaneTypeId("test.probe"), "Probe")
    {
    }
    void draw(lux::ui::Frame &, lux::ui::PaneDrawContext &) override
    {
        ++draws;
        ImGui::TextUnformatted("Real CPU Pane in an Editor Scene");
        ImGuiListClipper clipper;
        clipper.Begin(1000);
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
            {
                ImGui::Text("Row %d", row);
            }
        }
    }
};
} // namespace

int main(int argc, char **argv)
{
    using namespace lux;
    using namespace std::chrono_literals;
    using editor::ui::UIRenderSystem;
    const bool cost = argc > 1 && std::string_view(argv[1]) == "--cost";
    const std::uint64_t frame_count = cost ? 120 : 12;
    std::chrono::nanoseconds build_time{}, capture_time{}, retry_time{}, publish_time{}, wait_time{};
    std::size_t retry_calls{}, publish_calls{};
    const bool surface = argc > 1 && std::string_view(argv[1]) == "--surface";
    const bool multiple = argc > 1 && std::string_view(argv[1]) == "--multiple";
    window::GlfwRuntime platform;
    assert(platform.valid());
    std::unique_ptr<window::LuxWindow> window;
    if (surface)
    {
        window = std::make_unique<window::LuxWindow>(200, 150, "Unified UI native surface regression");
        assert(window->isInitialized());
        window->hide(true);
    }
    meta::ReflectionRegistry::initRegistry();
    render::RendererConfig renderer;
    renderer.validation = !cost;
    if (surface)
    {
        for (const auto *extension : window::LuxWindow::requiredVulkanInstanceExtensions())
        {
            renderer.instance_extensions.emplace_back(extension);
        }
    }
    renderer.feature_factories = {render::kUiRenderFeatureFactory};
    renderer.validation_message_sink = [](auto severity, auto message) {
        if (severity == 2)
        {
            std::cerr << message << '\n';
        }
    };
    auto created = render::RenderRuntime::create(std::move(renderer));
    assert(created);
    auto runtime = std::move(*created);
    object::ObjectMessageQueue messages;
    auto dispatcher = messages.dispatcherRef();
    editor::ui::UIRenderSystemConfig config;
    const auto registration = editor::ui::uiRenderSystemRegistration();
    auto metadata = scene::SceneMetaManager::build({.scene_systems = {registration}});
    assert(metadata);
    scene::SceneDescriptionBuilder builder;
    assert(builder.addSystem({1}, "UI", registration.type, 1, {}, 0));
    auto description = std::move(builder).buildResolved();
    assert(description);
    auto shared = std::make_shared<const scene::SceneDescription>(std::move(*description));
    std::array providers{
        scene::makeSceneCapabilityProvider<render::RenderRuntime>("runtime", "lux.render.runtime", *runtime),
        scene::makeSceneCapabilityProvider<object::ObjectDispatcherRef>("dispatcher", "lux.object.dispatcher",
                                                                        dispatcher),
        scene::makeSceneCapabilityProvider<editor::ui::UIRenderSystemConfig>("config", "lux.editor.ui.config", config)};
    scene::SceneCreateInfo input{shared,
                                 std::make_shared<const world::WorldDescription>(),
                                 std::make_shared<const simulation::SimulationDescription>(),
                                 *metadata,
                                 providers,
                                 simulation::ESimulationMode::DERIVATION};
    auto made = scene::SceneInstance::create(input);
    assert(made);
    auto instance = std::move(*made);
    assert(instance->simulation().seal());
    auto *ui = instance->findSceneSystem<UIRenderSystem>();
    auto *render_system = instance->findSceneSystem<scene::RenderSystem>();
    assert(ui && render_system == static_cast<scene::RenderSystem *>(ui));
    auto receipt = ui->resourceReceipt();
    auto executor = task::TaskExecutor::create({0, 1024});
    assert(executor);
    scene::SceneDriver driver(*executor);
    const auto pump = [&] {
        std::size_t controls = 4, programs = 1;
        assert(runtime->poll(16, controls, programs));
    };
    const auto until = [&](auto predicate) {
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (predicate())
            {
                return;
            }
            pump();
            std::this_thread::sleep_for(1ms);
        }
        assert(false && "The requested completion did not arrive");
    };
    until([&] { return receipt.status().state == render::ESceneResourceState::READY; });
    render::ViewConfig output{.extent = {200, 150}};
#if defined(_WIN32)
    if (surface)
    {
        output.output = render::NativeSurfaceOutput{reinterpret_cast<std::uintptr_t>(window->win32Handle())};
    }
#endif
    auto opened = ui->openView(output);
    assert(opened);
    auto view = std::move(*opened);
    until([&] { return view->status().state == render::EViewState::READY; });
    std::vector<std::unique_ptr<render::RenderView>> more_views;
    if (multiple)
    {
        // More draws in one GPU frame than FIF slots. A per-draw ring cursor
        // must not wrap and rewrite buffers still referenced by this frame.
        for (unsigned index{}; index < 4; ++index)
        {
            auto extra = ui->openView(output);
            assert(extra);
            more_views.push_back(std::move(*extra));
        }
        until([&] {
            return std::ranges::all_of(
                more_views, [](const auto &value) { return value->status().state == render::EViewState::READY; });
        });
    }
    ProbePane pane(dispatcher);
    auto pane_registration = ui->registerPane(pane);
    assert(pane_registration);

    for (std::uint64_t frame_index = 1; frame_index <= frame_count; ++frame_index)
    {
        const auto wait_begin = std::chrono::steady_clock::now();
        until([&] { return ui->canBuildFrame(); });
        const bool measure = cost && frame_index > 20;
        if (measure)
        {
            wait_time += std::chrono::steady_clock::now() - wait_begin;
        }
        const auto build_begin = std::chrono::steady_clock::now();
        auto frame = ui->beginFrame({{200, 150}, 1.F / 60.F});
        ui->drawPanes(frame);
        const auto build_end = std::chrono::steady_clock::now();
        assert(ui->finishFrame(frame));
        if (measure)
        {
            build_time += build_end - build_begin;
            capture_time += std::chrono::steady_clock::now() - build_end;
        }
        const auto draws = pane.draws;
        const auto captures = ui->capturedFrames();
        driver.invalidate(*instance);
        // Shared publication budget zero: do not recapture or repeat Pane work.
        for (unsigned retry = 0; retry != 3; ++retry)
        {
            scene::SceneAdvanceBudget turn{32, 1, 0};
            const auto begin = std::chrono::steady_clock::now();
            assert(driver.advance(*instance, begin, turn) == scene::ESceneProgress::PENDING);
            if (measure)
            {
                retry_time += std::chrono::steady_clock::now() - begin;
                ++retry_calls;
            }
            assert(turn.publications == 0 && pane.draws == draws && ui->capturedFrames() == captures);
            assert(!ui->canBuildFrame());
        }
        scene::SceneAdvanceBudget turn{32, 1, 1};
        until([&] {
            turn = {32, 1, 1};
            const auto begin = std::chrono::steady_clock::now();
            const auto completed = driver.advance(*instance, begin, turn) == scene::ESceneProgress::COMPLETE;
            if (measure)
            {
                publish_time += std::chrono::steady_clock::now() - begin;
                ++publish_calls;
            }
            return completed;
        });
        assert(turn.publications == 0);
        until([&] { return runtime->statistics().frames >= frame_index; });
        assert(instance->progress().clock.step_index == 0);
    }
    assert(ui->capturedFrames() == frame_count && pane.draws > 0);
    const auto ui_stop = std::chrono::steady_clock::now();
    if (cost)
    {
        const auto us = [](auto duration) { return std::chrono::duration<double, std::micro>(duration).count(); };
        std::cout << "MEASURE UI warmup=20 frames=100 rows=1000 width=200 height=150 views=1"
                  << " build_us=" << us(build_time) << " finish_capture_us=" << us(capture_time)
                  << " blocked_publish_calls=" << retry_calls << " blocked_publish_us=" << us(retry_time)
                  << " publish_calls=" << publish_calls << " publish_us=" << us(publish_time)
                  << " slot_wait_us=" << us(wait_time) << '\n';
        assert(retry_calls == 300);
    }
    // An abandoned CPU Frame is discarded, not silently submitted or captured.
    {
        auto abandoned = ui->beginFrame({{200, 150}, 1.F / 60.F});
        ImGui::TextUnformatted("discarded");
    }
    scene::SceneAdvanceBudget turn{32, 1, 1};
    static_cast<void>(driver.advance(*instance, std::chrono::steady_clock::now(), turn));
    assert(ui->canBuildFrame() && ui->capturedFrames() == frame_count && turn.publications == 1);

    auto image = view->acquireImage();
    assert(surface ? (!image && image.error().code == render::ERendererError::INVALID_ARGUMENT) : bool(image));
    // No explicit Binding shutdown: UI/Scene die while their output View and
    // (for sampled output) an image reference survive.
    instance.reset();
    const auto cpu_exit = std::chrono::steady_clock::now();
    assert(ImGui::GetCurrentContext() == nullptr);
    pane_registration->reset();
    more_views.clear();
    assert(receipt.status().state != render::ESceneResourceState::RETIRED);
    assert(view->beginClose());
    if (image)
    {
        for (unsigned i = 0; i < 4; ++i)
        {
            pump();
        }
        assert(runtime->statistics().views >= 1);
        *image = {};
    }
    view.reset();
    const auto view_exit = std::chrono::steady_clock::now();
    until([&] { return receipt.status().state == render::ESceneResourceState::RETIRED; });
    const auto retired_at = std::chrono::steady_clock::now();
    assert(receipt.status().failure.ok());
    if (cost)
    {
        const auto us = [](auto duration) { return std::chrono::duration<double, std::micro>(duration).count(); };
        std::cout << "MEASURE UI exit cpu_us=" << us(cpu_exit - ui_stop)
                  << " view_release_us=" << us(view_exit - ui_stop)
                  << " resource_retired_us=" << us(retired_at - ui_stop) << '\n';
    }
    assert(runtime->statistics().runtime_leases == 0 && runtime->statistics().validation_errors == 0);
    assert(runtime->beginClose());
    until([&] {
        std::size_t replies = 16, controls = 4, programs = 1;
        auto result = runtime->advanceClose(replies, controls, programs);
        assert(result);
        return *result == render::ERenderClose::COMPLETE;
    });
    assert(runtime->joinStopped());
    window.reset(); // Surface retirement and backend join have actually completed.
    messages.close();
    std::cout << "PASS actual UIRenderSystem/SceneDriver/UiRenderFeature: one capability owner, " << frame_count
              << " GPU frames, "
                 "budget retry without redraw, discarded frame, Context exits before View and GPU retirement\n";
}

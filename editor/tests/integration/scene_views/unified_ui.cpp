#include <lux/engine/scene/RenderSceneState.hpp>
#include "../../../../cmake/installed-consumers/common/UiTestContent.hpp"
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/scene/RenderResources.hpp>
#include "../../../../cmake/installed-consumers/common/RenderRegistration.hpp"
#include <imgui.h>
#include <lux/engine/editor/desktop/Presentation.hpp>
#include <lux/engine/editor/desktop/WindowOutput.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/editor/views/ViewportElement.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/scene/Builtin3DRenderIntegration.hpp>
#include <lux/engine/function/render/features/BuiltinFeatures.hpp>
#include <lux/engine/function/render/features/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/ui/rendering/RenderFeature.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

namespace
{
    std::atomic_bool gate_entered{}, gate_released{};
    lux::render::RenderFeatureRegistration gateRegistration()
    {
        using namespace lux;
        auto registration = render::kUiRenderRenderFeatureRegistration;
        auto& factory = registration.factory;
        factory.name = "UiTestGate";
        factory.descriptor.canonical_name = "test.ui.gate";
        factory.descriptor.type = render::featureId(factory.descriptor.canonical_name);
        factory.operation_count = 0;
        factory.register_ops_fn = nullptr;
        factory.unregister_ops_fn = nullptr;
        factory.create_fn = +[](void*, const void*, std::size_t) -> render::Expected<render::FeatureHandle> {
            gate_entered.store(true, std::memory_order_release);
            while (!gate_released.load(std::memory_order_acquire))
                gate_released.wait(false);
            return render::renderFailure<render::err::comm::RequestInvalid>();
        };
        registration.scene_configurable = false;
        return registration;
    }

    template <class Parent>
    concept SceneParent = requires(
        Parent& parent,
        lux::scene::SceneRuntime& scenes,
        lux::scene::SceneInstanceId scene,
        lux::scene::RenderResources& resources
    ) {
        lux::editor::views::ViewportElement::create(
            parent,
            lux::ui::ElementId{"view"},
            scenes,
            scene,
            resources,
            lux::system::SystemInstanceId{1},
            lux::simulation::ecs::NullEntity,
            lux::scene::ViewConfig{}
        );
    };
    static_assert(!SceneParent<lux::ui::Root>);
    static_assert(SceneParent<lux::ui::Pane> && SceneParent<lux::ui::Element>);
    struct TestRoot final : lux::ui::Root
    {
        explicit TestRoot(lux::object::ObjectDispatcherRef dispatcher, bool docking) : Root(dispatcher)
        {
            assert(initialize({.docking = docking}));
        }
        lux::editor::desktop::Presentation* presentation{};
        lux::cxx::expected<void, lux::ui::ECaptureError> drawDataReady(const lux::ui::DrawData& data) noexcept override
        {
            return presentation->captureDrawData(data);
        }
    };
    struct ProbePane final : lux::ui::Pane
    {
        std::size_t draws{};
        explicit ProbePane(lux::ui::Root& root)
            : Pane(root.dispatcherRef(), lux::ui::PaneId("probe"), lux::ui::PaneTypeId("test.probe"), "Probe")
        {
            setContent(probe_content_);
            ui_test::mount(root, *this);
        }

    public:
        TUiTestContent<ProbePane> probe_content_{*this};
        void drawTestContent(lux::ui::Element&) noexcept
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

int main(int argc, char** argv)
{
    using namespace lux;
    using namespace std::chrono_literals;
    const bool backpressure = argc > 1 && std::string_view(argv[1]) == "--backpressure";
    const bool scene_panes = argc > 1 && std::string_view(argv[1]) == "--scene-panes";
    const ui::Size logical_extent = scene_panes ? ui::Size{960, 600} : ui::Size{200, 150};
    const bool cost = argc > 1 && std::string_view(argv[1]) == "--cost";
    const std::uint64_t frame_count = cost ? 120 : 12;
    std::chrono::nanoseconds ui_time{}, input_time{}, wait_time{};
    std::size_t input_calls{};
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
    render::RendererConfig renderer;
    renderer.validation = !cost;
    if (surface)
    {
        for (const auto* extension : window::LuxWindow::requiredVulkanInstanceExtensions())
        {
            renderer.instance_extensions.emplace_back(extension);
        }
    }
    std::vector<lux::render::RenderFeatureRegistration> initial_features = {render::kUiRenderRenderFeatureRegistration};
    if (backpressure)
        initial_features.push_back(gateRegistration());
    if (scene_panes)
        initial_features.push_back(render::kViewCameraRenderFeatureRegistration);
    auto diagnostics = [](auto severity, auto message) {
        if (severity == 2)
        {
            std::cerr << message << '\n';
        }
    };
    auto created = render::RenderRuntime::create(std::move(renderer), std::move(diagnostics));
    assert(created);
    registerRenderFeatures(**created, std::move(initial_features));
    auto runtime = std::move(*created);
    auto execution = lux::process::ExecutionRuntime::create({1, 64, 64, {64}});
    assert(execution);
    lux::process::TaskScope tasks{*execution};
    auto made_resources = lux::scene::RenderResources::create(*runtime, tasks, execution->cpu());
    assert(made_resources);
    auto resources = std::move(*made_resources);

    auto messages_created = object::ObjectMessageQueue::create(64);
    assert(messages_created);
    auto messages = std::move(*messages_created);
    auto dispatcher = messages.dispatcherRef();
    auto root = std::make_unique<TestRoot>(dispatcher, !scene_panes);
    auto made_scenes = scene::SceneRuntime::create(*execution, {0, 1024});
    assert(made_scenes);
    auto scenes = std::move(*made_scenes);
    auto made_ui = editor::desktop::Presentation::create(*root, *execution, *scenes, *runtime, *resources);
    assert(made_ui);
    auto ui = std::move(*made_ui);
    root->presentation = ui.get();
    const lux::simulation::ecs::ComponentSchemaSet task_components{};
    const lux::simulation::SimulationSystemRegistry task_system_types;
    const auto pump = [&] {
        std::size_t controls = 4, programs = 1;
        assert(runtime->collectCompletions(16));
        assert(runtime->submitPending(controls, programs));
        assert(execution->collectCompletions());
        const auto advanced = scenes->driveFrame();
        assert(advanced && advanced->empty());
    };
    const auto until = [&](auto predicate, std::string_view waiting = "completion") {
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
        std::cerr << "Timed out waiting for " << waiting << std::endl;
        assert(false && "The requested completion did not arrive");
    };
    lux::scene::ViewConfig output{.extent = {200, 150}};
#if defined(_WIN32)
    if (surface)
    {
        output.output = lux::scene::NativeSurfaceOutput{reinterpret_cast<std::uintptr_t>(window->win32Handle())};
    }
#endif
    const auto open_output = [&](scene::ViewConfig config) {
        auto borrowed = scenes->borrowInstance(ui->sceneId());
        assert(borrowed);
        auto& registry = borrowed->get();
        const auto request = registry.create();
        registry.emplace<scene::RenderViewRequest>(
            request,
            system::SystemInstanceId{1},
            simulation::ecs::NullEntity,
            config
        );
        scene::RenderResourceId view;
        until([&] {
            const auto read = std::as_const(*scenes).borrowInstance(ui->sceneId());
            assert(read);
            const auto* result = read->get().try_get<scene::RenderViewResult>(request);
            if (result && result->view.isValid())
                view = result->view;
            return view.isValid();
        });
        assert(resources->retain(view));
        return view;
    };
    auto view = open_output(output);
    until([&] { return resources->observeView(view)->status.state == lux::scene::EViewState::READY; });
    std::vector<scene::RenderResourceId> more_views;
    if (multiple)
    {
        // More draws in one GPU frame than FIF slots. A per-draw ring cursor
        // must not wrap and rewrite buffers still referenced by this frame.
        for (unsigned index{}; index < 4; ++index)
        {
            more_views.push_back(open_output(output));
        }
        until([&] {
            return std::ranges::all_of(more_views, [&](const auto& value) {
                return resources->observeView(value)->status.state == lux::scene::EViewState::READY;
            });
        });
    }
    // An independent in-memory Scene, without any document, loader or SceneObjects.
    // Two elements borrow one ID. SceneRuntime is the only scheduler for content and UI.
    std::optional<scene::SceneInstanceLease> content, foreign_content;
    ui::Pane first_window(root->dispatcherRef(), ui::PaneId{"scene-one"}, ui::PaneTypeId{"test.scene"}, "Scene one");
    ui_test::mount(*root, first_window);
    ui::Pane second_window(root->dispatcherRef(), ui::PaneId{"scene-two"}, ui::PaneTypeId{"test.scene"}, "Scene two");
    ui_test::mount(*root, second_window);
    std::unique_ptr<lux::editor::views::ViewportElement> first_pane, second_pane;
    scene::RenderSceneReceipt content_receipt;
    if (scene_panes)
    {
        const auto reg = scene::builtinRenderSystemRegistration();
        const std::array regs{reg};
        scene::RenderSystemConfiguration empty_config;
        const auto& camera_feature = render::kViewCameraRenderFeatureRegistration;
        std::vector<std::byte> defaults;
        assert(camera_feature.configuration.portable.encode_default(defaults));
        empty_config.features.push_back(
            {camera_feature.factory.descriptor.type,
             std::move(defaults),
             std::string(camera_feature.configuration.schema),
             camera_feature.configuration.schema_version}
        );
        std::vector<std::byte> bytes;
        assert(reg.configuration.encode(&empty_config, bytes));
        scene::SceneDescriptionBuilder desc;
        assert(desc.addSystem(
            {1},
            "content",
            reg.type,
            reg.description->version,
            reg.description->configuration_schema_name,
            reg.description->configuration_schema_version,
            bytes
        ));
        auto built = std::move(desc).buildResolved();
        assert(built);
        auto bindings = scene::builtinRenderFeatureSceneBindings();
        const std::array deps{
            scene::makeSceneCapabilityProvider<render::RenderRuntime>("runtime", "lux.render.runtime", *runtime),
            scene::makeSceneCapabilityProvider<scene::RenderResources>("resources", "lux.render.resources", *resources),
            scene::makeSceneCapabilityProvider<scene::RenderFeatureSceneBindings>(
                "bindings",
                "lux.render.scene_bindings",
                bindings
            )
        };
        const auto shared = std::make_shared<const scene::SceneDescription>(std::move(*built));
        auto builder = scenes->builder();
        builder.setDescription(shared)
            .setWorld(std::make_shared<const world::WorldDescription>())
            .setSimulation(std::make_shared<const simulation::SimulationDescription>())
            .setRegistrations(task_components, task_system_types, regs)
            .setProviders(deps);
        auto created_content = builder.build();
        auto created_foreign = builder.build();
        assert(created_content && created_foreign);
        content = std::move(*created_content);
        foreign_content = std::move(*created_foreign);
        assert(scenes->pauseSimulation(content->id()) && scenes->pauseSimulation(foreign_content->id()));
        const auto* render = scene::RenderSceneState::find(scenes->borrowInstance(content->id())->get(), {1});
        content_receipt = resources->sceneReceipt(render->resource);
        until([&] { return content_receipt.status().state == scene::ESceneResourceState::READY; });
        const auto entity = scenes->borrowInstance(content->id())->get().create();
        scenes->borrowInstance(content->id())->get().emplace<scene::Camera>(entity);
        scenes->borrowInstance(content->id())->get().emplace<simulation::ecs::WorldTransform3D>(entity);
        const auto camera = entity;
        const auto count = resources->viewCount();
        const auto entity_count =
            scenes->borrowInstance(content->id())->get().storage<simulation::ecs::Entity>().size();
        auto wrong_render = lux::editor::views::ViewportElement::create(
            first_window,
            lux::ui::ElementId{"wrong-render"},
            *scenes,
            content->id(),
            *resources,
            {999},
            camera,
            {.extent = {64, 48}}
        );
        assert(!wrong_render && wrong_render.error().code == render::ERendererError::INVALID_ARGUMENT);
        auto wrong_camera = lux::editor::views::ViewportElement::create(
            first_window,
            lux::ui::ElementId{"wrong-camera"},
            *scenes,
            content->id(),
            *resources,
            {1},
            simulation::ecs::NullEntity,
            {.extent = {64, 48}}
        );
        assert(!wrong_camera && resources->viewCount() == count);
        assert(scenes->borrowInstance(content->id())->get().storage<simulation::ecs::Entity>().size() == entity_count);
        auto native_output = lux::editor::views::ViewportElement::create(
            first_window,
            lux::ui::ElementId{"native-output"},
            *scenes,
            content->id(),
            *resources,
            {1},
            camera,
            {{64, 48}, scene::NativeSurfaceOutput{1}}
        );
        assert(!native_output && resources->viewCount() == count);
        {
            auto abandoned = lux::editor::views::ViewportElement::create(
                first_window,
                lux::ui::ElementId{"abandoned"},
                *scenes,
                content->id(),
                *resources,
                {1},
                camera,
                {.extent = {16, 16}}
            );
            assert(abandoned && !(*abandoned)->view().isValid());
            abandoned->reset(); // No request has been adopted, and no UI maintenance is required.
            assert(scenes->driveFrame());
            assert(scenes->borrowInstance(content->id())->get().view<scene::RenderViewRequest>().empty());
            assert(resources->viewCount() == count);
        }
        auto one = lux::editor::views::ViewportElement::create(
            first_window,
            lux::ui::ElementId{"scene-one"},
            *scenes,
            content->id(),
            *resources,
            {1},
            camera,
            {.extent = {64, 48}}
        );
        auto two = lux::editor::views::ViewportElement::create(
            second_window,
            lux::ui::ElementId{"scene-two"},
            *scenes,
            content->id(),
            *resources,
            {1},
            camera,
            {.extent = {64, 48}}
        );
        assert(one && two);
        first_pane = std::move(*one);
        second_pane = std::move(*two);
        const auto borrowed_pose = first_pane->presentation().setCameraPose({}, {});
        assert(!borrowed_pose && borrowed_pose.error().code == render::ERendererError::INVALID_ARGUMENT);
        assert(scenes->borrowInstance(content->id())->get().valid(camera));
        first_window.setContent(*first_pane);
        second_window.setContent(*second_pane);
        root->setDockLayout({.left = "scene-one", .center = "scene-two", .left_width = 200.F});
        const auto replacement = scenes->borrowInstance(content->id())->get().create();
        scenes->borrowInstance(content->id())->get().emplace<scene::Camera>(replacement);
        scenes->borrowInstance(content->id())->get().emplace<simulation::ecs::WorldTransform3D>(replacement);
        assert(first_pane->setCamera(replacement));
        assert(first_pane->camera() == replacement);
        assert(second_pane->camera() == camera);
        scenes->borrowInstance(content->id())->get().destroy(entity);
        const auto reused = scenes->borrowInstance(content->id())->get().create();
        scenes->borrowInstance(content->id())->get().emplace<scene::Camera>(reused);
        scenes->borrowInstance(content->id())->get().emplace<simulation::ecs::WorldTransform3D>(reused);
        assert(!second_pane->setCamera(camera)); // Same slot, old Entity generation.
        assert(second_pane->setCamera(reused));
    }
    ProbePane pane(*root);

    for (std::uint64_t frame_index = 1; frame_index <= frame_count; ++frame_index)
    {
        const auto wait_begin = std::chrono::steady_clock::now();
        until([&] { return ui->tryAcquireDrawData() != nullptr; });
        const bool measure = cost && frame_index > 20;
        if (measure)
        {
            wait_time += std::chrono::steady_clock::now() - wait_begin;
        }
        if (content)
        {
            assert(scenes->driveFrame());
        }
        const auto build_begin = std::chrono::steady_clock::now();
        assert(root->update({logical_extent, 1.F / 60.F}, ui->tryAcquireDrawData()));
        const auto build_end = std::chrono::steady_clock::now();
        if (measure)
        {
            ui_time += build_end - build_begin;
        }
        const auto draws = pane.draws;
        const auto captures = ui->capturedFrames();
        // Publication may wait on transport, but polling never rebuilds UI input.
        until([&] {
            const auto begin = std::chrono::steady_clock::now();
            const auto submitted = ui->applySceneInput();
            assert(submitted);
            const bool completed = bool(submitted);
            if (measure)
            {
                input_time += std::chrono::steady_clock::now() - begin;
                ++input_calls;
            }
            return completed;
        });
        assert(pane.draws == draws && ui->capturedFrames() == captures);
        until([&] { return runtime->statistics().frames >= frame_index; });
    }
    assert(ui->capturedFrames() == frame_count && pane.draws > 0);
    if (scene_panes)
    {
        float framebuffer_scale = 1.F;
        const auto publish = [&] {
            assert(scenes->driveFrame());
            if (ui->tryAcquireDrawData() != nullptr)
            {
                assert(root->update(
                    {logical_extent, 1.F / 60.F, {framebuffer_scale, framebuffer_scale}},
                    ui->tryAcquireDrawData()
                ));
            }
            assert(ui->applySceneInput());
        };
        until(
            [&] {
                publish();
                return first_pane->image().image().isValid() && second_pane->image().image().isValid();
            },
            "two sampled outputs"
        );
        for (float scale : {1.25F, 1.5F, 2.F, 1.F})
        {
            framebuffer_scale = scale;
            auto diagnostic = std::chrono::steady_clock::now();
            until(
                [&] {
                    publish();
                    const auto size = first_pane->image().displayedSize();
                    const render::PixelExtent expected{
                        static_cast<std::uint32_t>(std::round(size.width * scale)),
                        static_cast<std::uint32_t>(std::round(size.height * scale))
                    };
                    if (std::chrono::steady_clock::now() - diagnostic > 2s)
                    {
                        const auto status = first_pane->observation().status;
                        std::cerr << "scale=" << scale << " expected=" << expected.width << ',' << expected.height
                                  << " requested=" << status.requested_extent.width << ','
                                  << status.requested_extent.height << " ready=" << status.ready_extent.width << ','
                                  << status.ready_extent.height << std::endl;
                        diagnostic = std::chrono::steady_clock::now();
                    }
                    const auto output = resources->viewOutput(first_pane->view());
                    const auto info =
                        output ? resources->outputInfo(*output)
                               : render::RenderResult<scene::RenderOutputInfo>{cxx::unexpected(output.error())};
                    const bool has_current_image =
                        info && info->extent == expected && info->texture == first_pane->image().image() &&
                        info->content.source.surface_generation == first_pane->observation().render_sequence;
                    return first_pane->observation().status.ready_extent == expected && has_current_image;
                },
                "framebuffer scale adoption"
            );
            assert(
                std::as_const(*scenes)
                    .borrowInstance(content->id())
                    ->get()
                    .ctx()
                    .get<std::reference_wrapper<const scene::SceneDriveSnapshot>>()
                    .get()
                    .time.step_index == 0
            );
        }
        std::puts("PASS lux::editor::views::ViewportElement framebuffer scale 125/150/200/100 percent: pixel extent "
                  "adopted without tick");
        assert(
            std::as_const(*scenes)
                .borrowInstance(content->id())
                ->get()
                .ctx()
                .get<std::reference_wrapper<const scene::SceneDriveSnapshot>>()
                .get()
                .time.step_index == 0
        );
        const auto first_view = first_pane->view();
        const auto first_closed = *resources->viewReceipt(first_view);
        const auto second_view = second_pane->view();
        first_pane.reset(); // Destructor cancels the request; GPU completion remains with resources.
        until(
            [&] {
                publish();
                return first_closed.status().status.state == scene::EViewState::CLOSED;
            },
            "first pane closure"
        );
        assert(second_pane->view() == second_view && second_pane->image().image().isValid());
        assert(std::as_const(*scenes).borrowInstance(content->id()));
        // Explicit close must report the receipt, even after the ID was released.
        const auto second_closed = *resources->viewReceipt(second_view);
        static_cast<void>(second_pane->close());
        assert(!second_pane->image().image().isValid());
        until(
            [&] {
                publish();
                return second_pane->close() == render::ERenderClose::COMPLETE;
            },
            "second pane closure"
        );
        assert(second_closed.status().status.state == scene::EViewState::CLOSED);
        const auto last_camera = second_pane->camera();
        second_pane.reset();
        assert(scenes->driveFrame());
        until([&] { return bool(scenes->borrowInstance(content->id())); });
        assert(scenes->borrowInstance(content->id())->get().view<scene::RenderViewRequest>().empty());
        auto expired_view = lux::editor::views::ViewportElement::create(
            first_window,
            ui::ElementId{"expired"},
            *scenes,
            content->id(),
            *resources,
            {1},
            last_camera,
            {.extent = {64, 48}}
        );
        assert(expired_view);
        first_window.setContent(**expired_view);
        assert(scenes->retireInstance(content->id()));
        content.reset();
        assert(!(*expired_view)->setCamera(last_camera));
        assert(root->update({logical_extent, 1.F / 60.F}, nullptr));
        assert(!(*expired_view)->image().image().isValid());
        expired_view->reset(
        ); // Revoking the ID before the Element must not leave a Registry borrow.        until([&] { return content_receipt.status().state == scene::ESceneResourceState::RETIRED; });
    }
    if (backpressure)
    {
        until([&] { return ui->tryAcquireDrawData() != nullptr; });
        const auto observed = resources->observeView(view);
        assert(observed);
        auto gate = runtime->control()->get().addFeatureRaw(
            observed->scene,
            runtime->features().typeId("UiTestGate"),
            std::span<const std::byte>{}
        );
        until([&] { return gate_entered.load(std::memory_order_acquire); });
        render::TRenderProgram<> filler;
        bool full{};
        for (unsigned index{}; index < 32; ++index)
        {
            render::RenderProgramSession::Builder builder(filler);
            builder.begin({});
            filler.kind = render::ERenderProgramKind::STATE_UPDATE;
            auto submitted = runtime->submit(filler);
            assert(submitted);
            if (*submitted == render::EFrameSubmit::BACKPRESSURED)
            {
                full = true;
                break;
            }
        }
        assert(full);
        assert(root->update({logical_extent, 1.F / 60.F}, ui->tryAcquireDrawData()));
        assert(ui->applySceneInput());
        const auto draws = pane.draws;
        const auto captures = ui->capturedFrames();
        for (unsigned retry{}; retry < 3; ++retry)
        {
            const auto advanced = scenes->driveFrame();
            assert(advanced && advanced->empty());
            assert(!scenes->borrowInstance(ui->sceneId()));
            assert(ui->applySceneInput());
            assert(pane.draws == draws && ui->capturedFrames() == captures);
        }
        gate_released.store(true, std::memory_order_release);
        gate_released.notify_all();
        until([&] { return gate.isReady() && runtime->statistics().frames >= captures; });
        assert(gate.tryResult() && !gate.tryResult()->get().error.ok());
        assert(pane.draws == draws && ui->capturedFrames() == captures);
        std::cout << "PASS UI FRAME real transport backpressure: fixed input, no redraw, eventual GPU frame\n";
    }
    const auto ui_stop = std::chrono::steady_clock::now();
    if (cost)
    {
        const auto us = [](auto duration) { return std::chrono::duration<double, std::micro>(duration).count(); };
        std::cout << "MEASURE UI warmup=20 frames=100 rows=1000 width=200 height=150 views=1"
                  << " root_update_us_total=" << us(ui_time) << " apply_input_calls=" << input_calls
                  << " apply_input_us_total=" << us(input_time) << " slot_wait_us_total=" << us(wait_time) << '\n';
    }
    // Invalid CPU input neither publishes nor consumes a capture slot.
    until([&] { return ui->tryAcquireDrawData() != nullptr; });
    assert(!root->update({{0, 150}, 1.F / 60.F}, ui->tryAcquireDrawData()));
    assert(ui->applySceneInput());
    assert(ui->tryAcquireDrawData() != nullptr && (scene_panes || ui->capturedFrames() == frame_count + backpressure));

    auto image = resources->viewOutput(view);
    assert(surface ? (!image && image.error().code == render::ERendererError::INVALID_ARGUMENT) : bool(image));
    if (image)
        assert(resources->retain(*image));
    const auto view_close = *resources->viewReceipt(view);
    // No explicit Binding shutdown: UI/Scene die while their output View and
    // (for sampled output) an image reference survive.
    ui.reset();
    if (foreign_content)
        assert(scenes->retireInstance(foreign_content->id()));
    foreign_content.reset();
    const auto cpu_exit = std::chrono::steady_clock::now();
    assert(ImGui::GetCurrentContext() == nullptr);
    for (auto id : more_views)
        resources->release(id);
    more_views.clear();
    resources->release(view);
    if (image)
    {
        for (unsigned i = 0; i < 4; ++i)
        {
            pump();
        }
        assert(view_close.status().status.state != lux::scene::EViewState::CLOSED);
        resources->release(*image);
    }
    const auto view_exit = std::chrono::steady_clock::now();
    until([&] { return resources->empty(); });
    const auto retired_at = std::chrono::steady_clock::now();
    if (cost)
    {
        const auto us = [](auto duration) { return std::chrono::duration<double, std::micro>(duration).count(); };
        std::cout << "MEASURE UI exit cpu_us=" << us(cpu_exit - ui_stop)
                  << " view_release_us=" << us(view_exit - ui_stop)
                  << " resource_retired_us=" << us(retired_at - ui_stop) << '\n';
    }
    assert(resources->empty() && runtime->statistics().validation_errors == 0);
    assert(tasks.join());
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
    std::cout << "PASS Presentation/RenderFeature through SceneRuntime: " << frame_count
              << " GPU frames, "
                 "publication without redraw, rejected frame, Scene exits before View and GPU retirement\n";
}

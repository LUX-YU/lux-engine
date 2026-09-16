#include "render_thread_checks.hpp"
#include "run_system.hpp"
#include <lux/engine/editor/Editor.hpp>
#include <lux/engine/editor/gui/scene/SceneDocumentProvider.hpp>
#include <lux/engine/editor/gui/shell/EditorWindow.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/function/render/client/genops/LightOperation.ops.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/TransformSystem.hpp>
#include <lux/engine/simulation/ecs/HierarchySchema.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>

int main(int argc, char **argv)
{
    using namespace lux;
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    assert(argc == 3);
    const bool failing = std::string_view(argv[2]) == "simulation-failure";
    const bool terminating = std::string_view(argv[2]) == "dynamic-terminal";
    meta::ReflectionRegistry::initRegistry();
    std::puts("dynamic: metadata registry initialized");
    // Load the real Scene UI provider DLL, including its generated configuration
    // metadata; the standalone driver still owns and advances the same business
    // type.
    const auto gui_provider = editor::gui::sceneDocumentProvider();
    assert(gui_provider.type == editor::scene::kSceneDocumentType);
    std::puts("dynamic: provider loaded");
    scene::initializeBuiltinRenderSystemMeta();
    render::initializeBuiltinRenderFeatureMeta();
    meta::ReflectionRegistry::drainPending();
    window::GlfwRuntime platform;
    assert(platform.valid());
    object::ObjectMessageQueue messages;
    auto runtime = process::ExecutionRuntime::create({2, 64, 64, {64}, process::BlockingSchedulerConfig{2, 64}});
    assert(runtime);
    auto source = stdexec::sync_wait(stdexec::then(stdexec::schedule(*runtime->blocking()),
                                                   [&]() noexcept { return editor::readProjectSource(argv[1]); }));
    assert(source && std::get<0>(*source));
    auto project = editor::Project::open(*std::get<0>(*source), *runtime->blocking(), messages.dispatcherRef());
    assert(project);
    std::puts("dynamic: project ready");
    editor::gui::WindowSpec spec;
    spec.visible = false;
    auto window = editor::gui::EditorWindow::create(messages.dispatcherRef(), spec);
    assert(window);
    editor::rendering::RendererConfig config;
    config.validation = true;
    auto renderer =
        editor::rendering::EditorRenderer::create((*window)->nativeWindow(), (*window)->uiSession(), config);
    assert(renderer);
    std::puts("dynamic: renderer ready");

    std::vector<simulation::ecs::ComponentSchema> schemas;
    const auto append = [&](auto values) { schemas.insert(schemas.end(), values.begin(), values.end()); };
    append(simulation::ecs::transformComponentSchemas());
    append(simulation::ecs::hierarchyComponentSchemas());
    append(simulation::ecs::visualComponentSchemas());
    append(scene::sceneRenderComponentSchemas());
    auto components = simulation::ecs::ComponentSchemaSet::build(std::move(schemas));
    assert(components);
    simulation::SimulationSystemRegistry systems;
    assert(systems.add(simulation::transformSystemRegistrations()));
    assert(systems.add(run_test::registration()));
    const auto render_systems = scene::builtinRenderSystemRegistrations();
    const auto features = render::builtinRenderFeatureRegistrations();
    const auto bindings = scene::builtinRenderFeatureSceneBindings();
    auto built = scene::SceneMetaManager::build({std::move(*components),
                                                 std::move(systems),
                                                 {render_systems.begin(), render_systems.end()},
                                                 {features.begin(), features.end()},
                                                 {bindings.begin(), bindings.end()}});
    if (!built)
    {
        std::printf("metadata failure=%u subject=%llu\n", unsigned(built.error().code),
                    static_cast<unsigned long long>(built.error().subject_hash));
    }
    assert(built);
    std::puts("dynamic: metadata ready");
    auto metadata = std::make_shared<const scene::SceneMetaManager>(std::move(*built));
    auto registration = editor::scene::sceneDocumentRegistration(*runtime, **renderer, metadata);
    const auto &asset = (*project)->manifest().assets.front();
    auto opening = registration.open(
        **project, {{{(*project)->manifest().id}, asset.id, std::string(editor::scene::kSceneDocumentType)},
                    "dynamic Run regression"});
    assert(opening);
    std::puts("dynamic: opening started");
    const auto deadline = Clock::now() + 30s;
    editor::rendering::EditorFramePacket packet;
    std::unique_ptr<editor::scene::RunViewLease> view;
    bool drawing = true;
    const auto pump = [&]
    {
        assert(Clock::now() < deadline);
        assert(runtime->drainMain(64));
        static_cast<void>(messages.dispatchPending(64));
        assert((*renderer)->poll(64));
        if (drawing && !packet.valid())
        {
            assert((*window)->beginFrame({{800, 600}, 0.01F, {1, 1}}));
            auto snapshot = (*window)->finishFrame();
            assert(snapshot);
            std::vector<editor::rendering::ViewImage> images;
            if (view)
            {
                auto image = view->view().acquireImage();
                if (image)
                {
                    images.push_back(std::move(*image));
                }
            }
            auto sealed = (*renderer)->sealFrame(*snapshot, images);
            assert(sealed);
            packet = std::move(*sealed);
        }
        if (packet.valid())
        {
            assert((*renderer)->trySubmitFrame(packet));
        }
        std::this_thread::sleep_for(1ms);
    };
    while (!(*opening)->settled())
    {
        pump();
        (*opening)->poll();
    }
    auto adopted = (*opening)->take();
    if (!adopted)
    {
        std::printf("open failed %s:%llu\n", adopted.error().domain.c_str(),
                    static_cast<unsigned long long>(adopted.error().reason));
    }
    assert(adopted);
    std::puts("dynamic: document adopted");
    opening->reset();
    auto &document = dynamic_cast<editor::scene::SceneEditor &>(**adopted);
    const auto tick = [&]
    {
        pump();
        editor::PollBudget budget;
        document.poll(budget);
    };
    const auto ready_resources = [&]
    {
        const auto snapshot = document.resources();
        if (!snapshot)
        {
            return false;
        }
        const auto &rows = snapshot->rows;
        return !rows.empty() && std::ranges::all_of(rows, [](const auto &row)
                                                    { return row.state == editor::scene::ESceneResourceState::READY; });
    };
    while (!ready_resources())
    {
        tick();
    }
    const auto history = document.historyView()->history;
    std::vector<simulation::ecs::Transform3D> author;
    for (const auto &row : document.objects())
    {
        author.push_back(*static_cast<const simulation::ecs::Transform3D *>(
            document.component(row.object, cxx::typeToken<simulation::ecs::Transform3D>())));
    }
    auto run = document.play(10ms);
    assert(run);
    while (document.runStatus().steps < 4)
    {
        tick();
        assert(document.runStatus().result);
    }
    auto opened = document.openRunView(*run, {{320, 240}, true, 2048});
    assert(opened);
    view = std::make_unique<editor::scene::RunViewLease>(std::move(*opened));
    editor::rendering::CameraFrame camera;
    camera.view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -8, 1};
    camera.projection = {0.8660254, 0, 0, 0, 0, -1.7320508, 0, 0, 0, 0, -1.0005, -1, 0, 0, -0.05, 0};
    camera.desired = {run->serial, 0, 1, 1};
    assert(view->view().setCamera(camera));
    const auto pause_at = Clock::now();
    assert(document.pauseRun(*run));
    while (document.runStatus().state != editor::scene::ERunState::PAUSED)
    {
        tick();
    }
    const auto pause_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - pause_at).count();
    const auto paused = document.runStatus();
    assert(paused.steps == run_test::steps && paused.completed.presentation == paused.steps);
    auto lease = (*renderer)->acquire();
    assert(lease);
    const auto light_ops = lease->features().ops<render::LightOperationIds>("Light");
    assert(light_ops.valid());
    render::LightControlClient light(lease->control(), light_ops);
    const auto check_backend = [&](std::uint64_t step)
    {
        // A Control reply can race Program adoption; query until it proves the
        // expected content while the producer is stably paused.
        for (;;)
        {
            auto query = light.stats({document.runStatus().render_scene});
            while (!query.isReady())
            {
                tick();
            }
            const auto result = query.tryResult();
            assert(result);
            const auto &stats = result->get();
            const auto total = stats.point_lights + stats.directional_lights + stats.spot_lights + stats.area_lights;
            if (total == 1 + step % 2)
            {
                std::printf("dynamic backend step=%llu lights=%u published=%llu "
                            "forwarded=%llu\n",
                            static_cast<unsigned long long>(step), total,
                            static_cast<unsigned long long>(document.runStatus().published_updates),
                            static_cast<unsigned long long>(document.runStatus().forwarded_updates));
                break;
            }
        }
    };
    check_backend(paused.steps);
    for (unsigned i = 0; i < 20; ++i)
    {
        tick();
        assert(run_test::steps == paused.steps);
    }
    const auto step_at = Clock::now();
    run_test::fail_step.store(failing);
    assert(document.stepRun(*run));
    while (document.runStatus().completed.presentation == paused.steps && document.runStatus().result)
    {
        tick();
    }
    const auto step_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - step_at).count();
    Clock::time_point stop_at;
    if (failing)
    {
        const auto failed = document.runStatus();
        assert(!failed.result && failed.result.error().domain == "run.simulation");
        assert(failed.failed_phase == editor::scene::ERunPhase::SIMULATION);
        assert(failed.steps == paused.steps + 1 && failed.elapsed == 10ms * failed.steps);
        assert(failed.completed.simulation == paused.steps && failed.completed.stable == paused.steps &&
               failed.completed.presentation == paused.steps);
        const auto *cause = std::any_cast<simulation::SimulationExecutionFailure>(&failed.result.error().cause);
        assert(cause && cause->code == simulation::ESimulationExecutionError::SYSTEM_TASK_FAILURE &&
               cause->system.value == 3);
        std::printf("JR02 simulation failure clock=%llu completed=%llu original_system=3\n",
                    static_cast<unsigned long long>(failed.steps),
                    static_cast<unsigned long long>(failed.completed.simulation));
        stop_at = step_at;
    }
    else
    {
        assert(document.runStatus().steps == paused.steps + 1 && run_test::steps == paused.steps + 1);
        check_backend(paused.steps + 1);
        assert(document.resumeRun(*run));
        // Stop Main's document/Binding consumption, while Main UI/GPU still
        // advances.
        const auto held_at = Clock::now();
        while (Clock::now() - held_at < 60ms)
        {
            pump();
        }
        const auto blocked_step = run_test::steps.load();
        const auto gpu = (*renderer)->statistics().gpu_completed;
        while (Clock::now() - held_at < 140ms)
        {
            pump();
        }
        assert(run_test::steps == blocked_step && (*renderer)->statistics().gpu_completed > gpu);
        std::printf("dynamic stable backpressure step=%llu GPU_delta=%llu; no next "
                    "Simulation step\n",
                    static_cast<unsigned long long>(blocked_step),
                    static_cast<unsigned long long>((*renderer)->statistics().gpu_completed - gpu));
        stop_at = Clock::now();
        if (terminating)
        {
            drawing = false;
            packet = {};
            lease->programs().progressDomain()->publishTerminalError(
                render::renderError<render::err::comm::ChannelStopping>());
            lease->programs().requestStop();
            editor::PollBudget budget;
            document.poll(budget); // Consumer stopping must wake the actual Run producer.
        }
        else
        {
            assert(document.stopRun(*run));
        }
    }
    while (run_test::destroyed_at_step.load(std::memory_order_acquire) == 0)
    {
        pump(); // Stop must wake the worker WITHOUT resuming Binding consumption.
    }
    const auto producer_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - stop_at).count();
    std::printf("dynamic: producer ended at step=%llu\n",
                static_cast<unsigned long long>(run_test::destroyed_at_step.load()));
    assert(view->view().beginClose());
    packet = {};
    while (*view->view().advanceClose() != editor::rendering::ERenderClose::COMPLETE)
    {
        tick();
    }
    view.reset();
    std::puts("dynamic: View closed");
    drawing = false;
    while (document.runStatus().state != editor::scene::ERunState::FINISHED &&
           document.runStatus().state != editor::scene::ERunState::FAILED)
    {
        tick();
    }
    const auto final = document.runStatus();
    std::puts("dynamic: Run settled");
    const auto close_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - stop_at).count();
    assert(final.steps == run_test::destroyed_at_step && final.elapsed.count() == run_test::elapsed_ns);
    assert(final.retained_resources == 0 && final.pending_updates == 0);
    assert(final.published_updates > 1);
    assert(final.published_updates == final.forwarded_updates + final.retired_updates);
    if (terminating)
    {
        assert(!final.result && final.retired_updates == 1);
        const auto *cause = std::any_cast<scene::SceneRenderBindingFailure>(&final.result.error().cause);
        assert(cause && cause->render.type == render::renderError<render::err::comm::ChannelStopping>().type);
        std::printf("JR01 full Run terminal steps=%llu forwarded=%llu retired=%llu "
                    "pins=0 original terminal preserved\n",
                    static_cast<unsigned long long>(final.steps),
                    static_cast<unsigned long long>(final.forwarded_updates),
                    static_cast<unsigned long long>(final.retired_updates));
    }
    else if (!failing)
    {
        assert(final.result && final.backpressure_count > 0 && final.publication_wait > 0ns);
    }
    const auto after = document.historyView()->history;
    assert(after.current == history.current && after.revision == history.revision && after.cursor == history.cursor);
    for (std::size_t i = 0; i < author.size(); ++i)
    {
        const auto *value = static_cast<const simulation::ecs::Transform3D *>(
            document.component(document.objects()[i].object, cxx::typeToken<simulation::ecs::Transform3D>()));
        assert(value->translation == author[i].translation);
    }
    std::printf("dynamic latency pause_stable_us=%lld step_completion_us=%lld "
                "stop_producer_us=%lld stop_resources_us=%lld steps=%llu "
                "published=%llu backpressure=%llu\n",
                static_cast<long long>(pause_us), static_cast<long long>(step_us), static_cast<long long>(producer_us),
                static_cast<long long>(close_us), static_cast<unsigned long long>(final.steps),
                static_cast<unsigned long long>(final.published_updates),
                static_cast<unsigned long long>(final.backpressure_count));
    document.requestClose();
    while (document.closeStatus().state != editor::ECloseState::CLOSED)
    {
        tick();
    }
    adopted->reset();
    packet = {};
    drawing = false;
    *lease = {};
    assert((*renderer)->statistics().runtime_leases == 0);
    assert((*renderer)->beginClose());
    while (*(*renderer)->advanceClose() != editor::rendering::ERenderClose::COMPLETE)
    {
        pump();
    }
    assert((*renderer)->statistics().validation_errors == 0);
    assert((*renderer)->joinStopped());
    renderer->reset();
    assert((*window)->closeAfterRendererStopped());
    window->reset();
    (*project)->requestClose();
    while (!*(*project)->advanceClose())
    {
        assert(runtime->drainMain(64));
    }
    project->reset();
    runtime->requestStop();
    assert(runtime->join());
    std::printf("PASS case=%s actual Simulation/Run/transport/GPU and author "
                "isolation, normal owner close\n",
                argv[2]);
}

#include "run_system.hpp"
#include "scene_structure_checks.hpp"
#include <cassert>
#include <cstdio>
#include <lux/engine/editor/gui/scene/SceneDocumentProvider.hpp>
#include <lux/engine/editor/project/Project.hpp>
#include <lux/engine/editor/scene/FieldEdit.hpp>
#include <lux/engine/function/render/features/BuiltinFeatures.hpp>
#include <lux/engine/function/render/features/genops/ForwardMeshOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/Grid3DOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/HighlightOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/LightOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/MaterialOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/MeshShadowOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/ShadowMapOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/render/RenderRuntime.hpp>
#include <lux/engine/scene/Builtin3DRenderIntegration.hpp>
#include <lux/engine/scene/SceneRenderSchema.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/simulation/ecs/VisualSchema.hpp>
#include <thread>

#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/function/render/features/genops/LightOperation.ops.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/scene/MeshQuerySystem.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
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
    const bool failing =
        std::string_view(argv[2]) == "simulation-failure" || std::string_view(argv[2]) == "failure-closing-terminal";
    const bool terminating = std::string_view(argv[2]) == "dynamic-terminal";
    const bool observing = std::string_view(argv[2]) == "dynamic-observer";
    const bool closing_terminal =
        std::string_view(argv[2]) == "closing-terminal" || std::string_view(argv[2]) == "failure-closing-terminal";
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
    auto runtime = process::ExecutionRuntime::create({1, 64, 64, {64}, process::BlockingSchedulerConfig{2, 64}});
    assert(runtime);
    auto source = stdexec::sync_wait(stdexec::then(stdexec::schedule(*runtime->blocking()),
                                                   [&]() noexcept { return editor::readProjectSource(argv[1]); }));
    assert(source && std::get<0>(*source));
    process::TaskScope tasks;
    auto project = editor::Project::open(*std::get<0>(*source), *runtime->blocking(), tasks, messages.dispatcherRef());
    assert(project);
    std::puts("dynamic: project ready");
    render::RendererConfig config;
    config.validation = std::string_view(argv[2]) != "dynamic-cost";
    config.validation_message_sink = [](auto severity, auto message) {
        if (severity == 2)
        {
            std::fprintf(stderr, "%.*s\n", int(message.size()), message.data());
        }
    };
    config.feature_factories = {
        render::kViewCameraFeatureFactory, render::kMaterialFeatureFactory,    render::kMeshStackFeatureFactory,
        render::kLightFeatureFactory,      render::kForwardMeshFeatureFactory, render::kShadowMapFeatureFactory,
        render::kMeshShadowFeatureFactory, render::kHighlightFeatureFactory,   render::kGrid3DFeatureFactory};
    auto renderer = render::RenderRuntime::create(std::move(config));
    assert(renderer);
    std::puts("dynamic: renderer ready");

    std::vector<simulation::ecs::ComponentSchema> schemas;
    const auto append = [&](auto values) { schemas.insert(schemas.end(), values.begin(), values.end()); };
    append(simulation::ecs::transformComponentSchemas());
    append(simulation::ecs::hierarchyComponentSchemas());
    append(simulation::ecs::visualComponentSchemas());
    append(scene::sceneRenderComponentSchemas());
    append(scene::worldLoadingComponentSchemas());
    auto components = simulation::ecs::ComponentSchemaSet::build(std::move(schemas));
    assert(components);
    simulation::SimulationSystemRegistry systems;
    assert(systems.add(simulation::transformSystemRegistrations()));
    assert(systems.add(run_test::registration()));
    const auto render_systems = scene::builtinRenderSystemRegistrations();
    const auto features = render::builtinRenderFeatureRegistrations();
    const auto bindings = scene::builtinRenderFeatureSceneBindings();
    std::vector<scene::SceneSystemRegistration> registrations{render_systems.begin(), render_systems.end()};
    registrations.push_back(scene::builtinMeshQuerySystemRegistration());
    registrations.push_back(scene::worldLoadingSystemRegistration());
    auto built = scene::SceneMetaManager::build({std::move(*components), std::move(systems), std::move(registrations)});
    if (!built)
    {
        std::printf("metadata failure=%u subject=%llu\n", unsigned(built.error().code),
                    static_cast<unsigned long long>(built.error().subject_hash));
    }
    assert(built);
    std::puts("dynamic: metadata ready");
    auto render_meta = scene::RenderSystemMetadata::build(*built, {features.begin(), features.end()}, bindings);
    assert(render_meta);
    editor::scene::SceneEditorMetadata metadata{
        std::make_shared<const scene::SceneMetaManager>(std::move(*built)),
        std::make_shared<const scene::RenderSystemMetadata>(std::move(*render_meta))};
    auto registration = editor::scene::sceneDocumentRegistration(*runtime, **renderer, metadata);
    const auto &asset = (*project)->manifest().assets.front();
    auto opening = registration.open(
        **project, {{{(*project)->manifest().id}, asset.id, std::string(editor::scene::kSceneDocumentType)},
                    "dynamic Run regression"});
    assert(opening);
    std::puts("dynamic: opening started");
    const auto deadline = Clock::now() + 30s;
    render::RenderProgram<> packet;
    bool frame_pending{};
    std::unique_ptr<render::RenderView> view;
    bool drawing = true;
    const auto pump = [&] {
        assert(Clock::now() < deadline);
        assert(runtime->drainMain(64));
        static_cast<void>(messages.dispatchPending(64));
        std::size_t controls = 64, programs = 8;
        assert((*renderer)->poll(64, controls, programs));
        if (drawing && (*renderer)->status().state == render::ERenderRuntimeState::ACTIVE)
        {
            if (!frame_pending)
            {
                render::RenderProgramBuilder<> builder(packet);
                builder.begin();
                packet.kind = render::ERenderProgramKind::Frame;
                frame_pending = true;
            }
            const auto submitted = (*renderer)->submit(packet);
            assert(submitted);
            frame_pending = *submitted == render::EFrameSubmit::BACKPRESSURED;
        }
        std::this_thread::sleep_for(1ms);
    };
    while (!(*opening)->settled())
    {
        pump();
        editor::PollBudget budget;
        (*opening)->poll(budget);
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
    const auto tick = [&] {
        pump();
        editor::PollBudget budget;
        document.poll(budget);
    };
    const auto ready_resources = [&] {
        const auto snapshot = document.resources();
        if (!snapshot)
        {
            return false;
        }
        const auto &rows = snapshot->rows;
        return !rows.empty() &&
               std::ranges::all_of(rows, [](const auto &row) { return row.state == scene::ERenderAssetState::READY; });
    };
    while (!observing && !ready_resources())
    {
        tick();
    }
    if (std::string_view(argv[2]) == "dynamic-run")
    {
        checkSceneStructure(document, false);
        using Transform = simulation::ecs::Transform3D;
        const auto object = document.objects().front().object;
        const auto original =
            static_cast<const Transform *>(document.component(object, cxx::typeToken<Transform>()))->translation;
        const Eigen::Vector3d changed = original + Eigen::Vector3d{2, 0, 0};
        assert(document.setField<Transform>(
            *document.writeTarget(object), "Transform3D.translation", "Translation",
            [](auto &value) { return &value.translation; }, changed));
        assert(document.undo());
        assert(document.redo());
        assert(document.undo());
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
    auto opened = document.openRunView(*run, {{320, 240}, render::SampledOutput{}, 2048});
    assert(opened);
    view = std::move(*opened);
    while (!view->handle().isValid())
    {
        tick();
    }
    const auto camera = document.viewportCamera();
    assert(camera);
    assert(document.bindCamera(*camera, view->id()));
    const auto pause_at = Clock::now();
    assert(document.pauseRun(*run));
    while (document.runStatus().state != editor::scene::ERunState::PAUSED)
    {
        tick();
    }
    const auto pause_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - pause_at).count();
    auto paused = document.runStatus();
    assert(paused.steps == run_test::steps && paused.completed.publication == paused.steps);
    auto control = (*renderer)->control();
    assert(control);
    const auto light_ops = (*renderer)->features().ops<render::LightOperationIds>("Light");
    assert(light_ops.valid());
    render::LightControlClient light(control->get(), light_ops);
    const auto check_backend = [&](std::uint64_t step) {
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
    if (observing)
    {
        const auto mesh_ops = (*renderer)->features().ops<render::MeshStackOperationIds>("StandardMeshStack");
        assert(mesh_ops.valid());
        render::MeshStackControlClient meshes(control->get(), mesh_ops);
        const auto check_meshes = [&](std::uint32_t expected) {
            for (;;)
            {
                auto request = meshes.stats({document.runStatus().render_scene});
                while (!request.isReady())
                {
                    tick();
                }
                auto result = request.tryResult();
                assert(result);
                if (result->get().alive_instances == expected)
                {
                    break;
                }
                tick();
            }
        };
        while (!ready_resources())
        {
            tick();
        }
        assert(document.objects().size() == 2 && document.resources()->rows.size() == 1);
        check_meshes(1);
        const auto loaded_step = paused.steps + 1;
        run_test::observer_partition = 1;
        assert(document.stepRun(*run));
        while (document.runStatus().completed.publication < loaded_step || document.objects().size() != 4 ||
               !ready_resources() || document.resources()->rows.size() != 3)
        {
            tick();
            assert(document.runStatus().result);
        }
        assert(document.resources()->rows.size() == 3);
        check_meshes(3);
        run_test::observer_partition = UINT32_MAX;
        assert(document.stepRun(*run));
        while (document.runStatus().completed.publication < loaded_step + 1 || document.objects().size() != 2)
        {
            tick();
            assert(document.runStatus().result);
        }
        check_meshes(1);
        paused = document.runStatus();
        assert(paused.state == editor::scene::ERunState::PAUSED && paused.steps == loaded_step + 1);
        std::puts("PASS WorldLoading -> real resource reads/uploads -> Mesh instances 1/3/1, author stayed intact");
    }
    const auto step_at = Clock::now();
    run_test::fail_step.store(failing);
    assert(document.stepRun(*run));
    while (document.runStatus().completed.publication == paused.steps && document.runStatus().result)
    {
        tick();
    }
    const auto step_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - step_at).count();
    Clock::time_point stop_at;
    if (failing)
    {
        const auto failed = document.runStatus();
        assert(!failed.result && failed.result.error().domain == "run.advance");
        assert(failed.failed_phase == editor::scene::ERunPhase::SIMULATION);
        assert(failed.steps == paused.steps + 1 && failed.elapsed == 10ms * failed.steps);
        assert(failed.completed.simulation == paused.steps && failed.completed.stable == paused.steps &&
               failed.completed.publication == paused.steps);
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
        // Continue Main simulation turns, but give this document zero Program
        // submissions. It must hold one prepared update and return to the UI.
        const auto held_at = Clock::now();
        const auto held_tick = [&] {
            pump();
            editor::PollBudget budget;
            budget.render_programs = 0;
            document.poll(budget);
        };
        while (!document.runStatus().pending_updates)
        {
            held_tick();
        }
        const auto blocked_step = run_test::steps.load();
        const auto gpu = (*renderer)->statistics().gpu_completed;
        while (Clock::now() - held_at < 140ms)
        {
            held_tick();
        }
        assert(run_test::steps == blocked_step && (*renderer)->statistics().gpu_completed > gpu);
        std::printf("dynamic retained update step=%llu GPU_delta=%llu Main returns without next step\n",
                    static_cast<unsigned long long>(blocked_step),
                    static_cast<unsigned long long>((*renderer)->statistics().gpu_completed - gpu));
        stop_at = Clock::now();
        if (terminating)
        {
            drawing = false;
            packet = {};
            control->get().requestStop(); // Real shared channel termination, not physical device loss.
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
        pump();
        editor::PollBudget budget;
        budget.render_programs = 0;
        document.poll(budget); // Stop releases the Main-owned World even with no submit allowance.
    }
    const auto system_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - stop_at).count();
    std::printf("dynamic: Simulation system destroyed at step=%llu\n",
                static_cast<unsigned long long>(run_test::destroyed_at_step.load()));
    // Main already owns the final clock and result before it releases the World.
    while (document.runStatus().steps != run_test::destroyed_at_step.load())
    {
        tick();
    }
    const auto result_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - stop_at).count();
    std::puts("dynamic: Main retained the final result before World release");
    // CPU Scene has already exited. The View is an independent Runtime use.
    assert(document.runStatus().state == editor::scene::ERunState::STOPPING);
    document.unbindCamera(*camera, view->id());
    assert(view->beginClose());
    packet = {};
    frame_pending = false;
    drawing = false;
    if (closing_terminal)
    {
        // Release intent is established, no Main retirement has run yet. This
        // replaces the removed Binding/drain observation with its actual owner.
        assert(view->status().state == render::EViewState::CLOSING);
        assert((*renderer)->status().state == render::ERenderRuntimeState::ACTIVE);
        assert(document.runStatus().retained_resources != 0);
        assert(bool(document.runStatus().result) == !failing);
        control->get().requestStop();
    }
    while (*view->advanceClose() != render::ERenderClose::COMPLETE)
    {
        tick();
    }
    view.reset();
    const auto view_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - stop_at).count();
    std::puts("dynamic: View closed after CPU Scene destruction");
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
    bool result_accurate = true;
    if (closing_terminal)
    {
        result_accurate = final.state == editor::scene::ERunState::FAILED && !final.result;
        if (result_accurate && failing)
        {
            const auto *cause = std::any_cast<simulation::SimulationExecutionFailure>(&final.result.error().cause);
            result_accurate = final.failed_phase == editor::scene::ERunPhase::SIMULATION &&
                              final.result.error().domain == "run.advance" && cause &&
                              cause->code == simulation::ESimulationExecutionError::SYSTEM_TASK_FAILURE &&
                              cause->system.value == 3;
        }
        else if (result_accurate)
        {
            const auto *cause = std::any_cast<render::RenderError>(&final.result.error().cause);
            const auto expected = render::renderError<render::err::comm::ChannelStopping>();
            result_accurate = final.result.error().domain == "run.render" &&
                              final.failed_phase == editor::scene::ERunPhase::PUBLICATION && cause &&
                              cause->type == expected.type && cause->args == expected.args;
        }
        std::printf("JR03 after terminal: state=%u result=%s domain=%s pins=%zu pending=%u "
                    "retired=%llu accurate=%u\n",
                    unsigned(final.state), final.result ? "success" : "failure",
                    final.result ? "none" : final.result.error().domain.c_str(), final.retained_resources,
                    final.pending_updates, static_cast<unsigned long long>(final.retired_updates), result_accurate);
    }
    else if (terminating)
    {
        assert(!final.result && final.retired_updates == 1);
        const auto *cause = std::any_cast<render::RenderError>(&final.result.error().cause);
        if (!cause)
        {
            const auto *stage = std::any_cast<scene::SceneExecutionFailure>(&final.result.error().cause);
            assert(stage && stage->system.value == 2 && stage->code == scene::ESceneExecutionError::SYSTEM_FAILURE);
            cause = std::any_cast<render::RenderError>(&stage->cause);
        }
        const auto expected = render::renderError<render::err::comm::ChannelStopping>();
        assert(cause && cause->type == expected.type && cause->args == expected.args);
        std::printf("JR01 full Run terminal steps=%llu forwarded=%llu retired=%llu "
                    "pins=0 original terminal preserved\n",
                    static_cast<unsigned long long>(final.steps),
                    static_cast<unsigned long long>(final.forwarded_updates),
                    static_cast<unsigned long long>(final.retired_updates));
    }
    else if (!failing)
    {
        assert(final.result && final.update_high_water == 1 && final.publication_wait > 0ns);
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
                "stop_system_us=%lld stop_result_us=%lld stop_resources_us=%lld steps=%llu "
                "published=%llu backpressure=%llu work_ns=%lld publication_wait_ns=%lld longest_advance_ns=%lld\n",
                static_cast<long long>(pause_us), static_cast<long long>(step_us), static_cast<long long>(system_us),
                static_cast<long long>(result_us), static_cast<long long>(close_us),
                static_cast<unsigned long long>(final.steps), static_cast<unsigned long long>(final.published_updates),
                static_cast<unsigned long long>(final.backpressure_count),
                static_cast<long long>(final.simulation_work.count()),
                static_cast<long long>(final.publication_wait.count()),
                static_cast<long long>(final.longest_advance.count()));
    std::printf("dynamic retirement observations_us: world=%lld result=%lld view=%lld resources=%lld\n",
                static_cast<long long>(system_us), static_cast<long long>(result_us), static_cast<long long>(view_us),
                static_cast<long long>(close_us));
    document.requestClose();
    while (document.closeStatus().state != editor::ECloseState::CLOSED)
    {
        tick();
    }
    adopted->reset();
    packet = {};
    drawing = false;
    // Destructors record release intent. Runtime maintenance releases nested
    // resource uses on subsequent bounded turns, including after backend exit.
    while ((*renderer)->statistics().runtime_leases != 0)
    {
        pump();
    }
    assert((*renderer)->statistics().views == 0);
    assert((*renderer)->beginClose());
    for (;;)
    {
        std::size_t replies = 64, controls = 64, programs = 8;
        const auto closed = (*renderer)->advanceClose(replies, controls, programs);
        assert(closed);
        if (*closed == render::ERenderClose::COMPLETE)
        {
            break;
        }
        pump();
    }
    assert((*renderer)->statistics().validation_errors == 0);
    assert((*renderer)->joinStopped());
    renderer->reset();
    (*project)->requestClose();
    while (!*(*project)->advanceClose())
    {
        assert(runtime->drainMain(64));
    }
    project->reset();
    assert(stdexec::sync_wait(tasks.close()));
    runtime->requestStop();
    assert(runtime->join());
    if (!result_accurate)
    {
        std::puts("FAIL JR03: normal drain followed by backend failure reported success; "
                  "World/View/Scene resources still retired, owner cleanup completed");
        return 2;
    }
    std::printf("PASS case=%s actual Simulation/Run/transport/GPU and author "
                "isolation, normal owner close\n",
                argv[2]);
}

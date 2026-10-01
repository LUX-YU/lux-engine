#pragma once
#include "ToolTestAccess.hpp"
#include "menu_checks.hpp"
#include <lux/engine/editor/views/ViewportElement.hpp>

#include "entity_checks.hpp"
#include <lux/engine/scene/RenderResources.hpp>
#include <lux/engine/ui/ImageElement.hpp>
#include "RenderOutputProbe.hpp"

#include <cassert>
#include <cstdio>

#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/function/render/features/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/render/RenderRuntime.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>

// A real Pane records the sampled output during drawing. Hiding it immediately
// afterwards must not revoke the render packet's image lease.
class RunImageProbe final : public lux::ui::Pane
{
public:
    RunImageProbe(
        lux::editor::scene::SceneEditor& scene,
        lux::scene::RenderResources& resources,
        RenderOutputProbe& captured
    )
        : lux::ui::Pane(
              scene,
              lux::ui::PaneId{"test.run-image"},
              lux::ui::PaneTypeId{"test.observer"},
              "Run image observer"
          ),
          scene_(scene), resources_(resources), captured_(captured)
    {}
    void requestClose() noexcept
    {
        closing_ = true;
        setVisible(false);
        captured_ = {};
    }
    void update() noexcept override
    {
        const auto run = scene_.runStatus();
        const bool is_active = run.state == lux::editor::scene::EPlaybackState::RUNNING ||
                               run.state == lux::editor::scene::EPlaybackState::PAUSED;
        if (closing_ || (viewport_ && !is_active))
        {
            if (viewport_)
            {
                static_cast<void>(viewport_->close());
                receipt_ = viewport_->observation();
            }
            return;
        }
        if (!viewport_ && run.state == lux::editor::scene::EPlaybackState::RUNNING)
        {
            auto camera = toolTest(scene_).viewportCamera();
            if (!camera)
                return;
            auto opened = lux::editor::views::ViewportElement::create(
                *this,
                lux::ui::ElementId{"test.run-image.image"},
                testSceneRuntime(scene_),
                toolTest(scene_).instance(),
                resources_,
                toolTest(scene_).selectedRenderSystem(),
                *camera,
                {.extent = {128, 96}}
            );
            if (opened)
            {
                viewport_ = std::move(*opened);
                setContent(*viewport_);
            }
        }
        if (captured_.valid())
        {
            setVisible(false);
            return;
        }
        setVisible(true);
        if (viewport_ && viewport_->image().image().isValid())
        {
            const auto image = resources_.viewOutput(viewport_->view());
            if (image)
                captured_ = RenderOutputProbe(resources_, *image);
        }
    }
    lux::editor::CloseStatus closeStatus() const
    {
        const bool complete = !viewport_ || receipt_.status.state == lux::scene::EViewState::CLOSED;
        return {
            closing_ ? (complete ? lux::editor::ECloseState::CLOSED : lux::editor::ECloseState::CLOSING)
                     : lux::editor::ECloseState::OPEN,
            {}
        };
    }

private:
    lux::editor::scene::SceneEditor& scene_;
    lux::scene::RenderResources& resources_;
    RenderOutputProbe& captured_;
    lux::scene::ViewObservation receipt_;
    std::unique_ptr<lux::editor::views::ViewportElement> viewport_;
    bool closing_{};
};

struct ScenePlaybackChecks final
{
    lux::editor::scene::RunId first;
    lux::editor::scene::StartRunId first_request;
    lux::editor::editing::HistorySnapshot author;
    std::optional<lux::editor::sessions::PersistedState> author_persisted;
    lux::editor::editing::HistoryId pause_history;
    std::uint64_t paused_step{};
    std::uint64_t first_frame{};
    std::size_t phase{}, frames{};
    std::size_t polls{};
    RenderOutputProbe held_image;
    std::unique_ptr<RunImageProbe> observer;
    lux::render::RenderRuntime* runtime{};
    lux::render::MeshStackOperationIds mesh_ops;
    lux::render::RenderSceneId author_scene;
    lux::render::TRenderRequest<lux::render::MeshStackStatsReply> run_mesh, author_mesh;
    bool initial_adopted{};
    lux::simulation::ecs::Entity selected{lux::simulation::ecs::NullEntity};
    Eigen::Vector3d author_translation, paused_translation;
    lux::editor::scene::SceneWriteTarget stale_pause_target;
    lux::editor::editing::StateId captured_state;

    void query(lux::editor::scene::SceneEditor& scene)
    {
        lux::render::MeshStackControlClient client(runtime->control()->get(), mesh_ops);
        run_mesh = client.stats({scene.runStatus().render_scene});
        author_mesh = client.stats({author_scene});
    }

    void begin(
        lux::editor::scene::SceneEditor& scene,
        lux::render::RenderRuntime& renderer,
        lux::scene::RenderResources& resources
    )
    {
        selected = sceneObjects(scene).front().object;
        assert(toolTest(scene).select(selected));
        author_translation =
            static_cast<const lux::simulation::ecs::Transform3D*>(
                toolTest(scene).component(selected, lux::cxx::typeToken<lux::simulation::ecs::Transform3D>())
            )
                ->translation;
        author = scene.historyView()->history;
        author_persisted = scene.persistedState();
        first_frame = renderer.statistics().frames;
        captured_state = author.current;
        runtime = &renderer;
        author_scene = *toolTest(scene).renderScene();
        mesh_ops = runtime->features().ops<lux::render::MeshStackOperationIds>("StandardMeshStack");
        assert(mesh_ops.valid());
        observer = std::make_unique<RunImageProbe>(scene, resources, held_image);
        lux::render::MeshStackControlClient client(runtime->control()->get(), mesh_ops);
        author_mesh = client.stats({author_scene});
    }

    bool poll(lux::editor::scene::SceneEditor& scene, lux::render::RenderRuntime& renderer)
    {
        using namespace lux::editor;
        if (first_request.serial == 0)
        {
            // Run suspends author maintenance. Establish the author GPU oracle
            // before Play instead of assuming resource readiness means its
            // StateUpdate has already been prepared and adopted.
            if (!author_mesh.isReady())
            {
                return false;
            }
            assert(author_mesh.tryResult());
            const auto count = author_mesh.tryResult()->get().alive_instances;
            if (count != 3)
            {
                lux::render::MeshStackControlClient client(runtime->control()->get(), mesh_ops);
                author_mesh = client.stats({author_scene});
                return false;
            }
            std::puts("Run precondition: author backend adopted three mesh instances before Play");
            const auto invalid_delta = scene.play(std::chrono::nanoseconds(0));
            assert(!invalid_delta && invalid_delta.error().code == EEditorError::INVALID_ARGUMENT);
            auto started = scene.play(std::chrono::milliseconds(10));
            assert(started);
            first_request = *started;
            const auto duplicate = scene.play();
            assert(!duplicate && duplicate.error().code == EEditorError::BUSY);
            return false;
        }
        const auto run = scene.runStatus();
        if (!first.valid() && run.id.valid())
        {
            assert(run.request == first_request);
            first = run.id;
        }
        if (++polls % 120 == 0)
        {
            std::printf(
                "Run trace phase=%zu state=%u steps=%llu render_views=%zu held=%d\n",
                phase,
                unsigned(run.state),
                static_cast<unsigned long long>(run.steps),
                static_cast<Editor&>(scene.root()).context().renderResources().viewCount(),
                held_image.valid()
            );
        }
        if (!run.result)
        {
            std::fprintf(
                stderr,
                "Run failure: %s reason=%llu %s\n",
                run.result.error().domain.c_str(),
                static_cast<unsigned long long>(run.result.error().reason),
                run.result.error().message.c_str()
            );
        }
        assert(run.result);
        const auto current = scene.historyView()->history;
        if (current.current.history == author.current.history)
        {
            assert(
                current.current == author.current && scene.persistedState() == author_persisted &&
                current.revision == author.revision && current.cursor == author.cursor
            );
        }
        if (phase == 0 && run.state == scene::EPlaybackState::RUNNING && run.steps >= 3)
        {
            if (!initial_adopted)
            {
                if (!run_mesh.valid())
                {
                    query(scene);
                    return false;
                }
                if (!run_mesh.isReady() || !author_mesh.isReady())
                {
                    return false;
                }
                assert(run_mesh.tryResult() && author_mesh.tryResult());
                if (run_mesh.tryResult()->get().alive_instances != 3)
                {
                    query(scene);
                    return false;
                }
                assert(author_mesh.tryResult()->get().alive_instances == 3);
                assert(
                    run.render_scene != author_scene && run.render_scene == *toolTest(scene).renderScene() &&
                    run.retained_resources == 3
                );
                initial_adopted = true;
                held_image = {}; // Observe a frame after the Run's content was adopted.
                return false;
            }
            bool completed{};
            if (held_image.valid())
            {
                auto evidence = held_image.evidence();
                completed = evidence && evidence->evidence == lux::scene::EImageEvidence::GPU_COMPLETE;
            }
            if (!completed)
            {
                return false;
            }
            assert(scene.pauseRun(first));
            phase = 1;
        }
        else if (phase == 1 && run.state == scene::EPlaybackState::PAUSED)
        {
            pause_history = scene.historyId();
            assert(pause_history != author.current.history);
            std::size_t notices{};
            auto notification =
                lux::object::LuxObject::connect(
                    std::addressof(scene),
                    &scene::SceneEditor::componentChanged,
                    [&](const auto&) noexcept {
                        for (const auto& result : {scene.resumeRun(first), scene.stepRun(first), scene.stopRun(first)})
                        {
                            assert(!result && result.error().code == EEditorError::BUSY);
                        }
                        assert(
                            scene.runStatus().state == scene::EPlaybackState::PAUSED &&
                            scene.historyId() == pause_history
                        );
                        ++notices;
                    }
                ).value();
            const auto object = sceneObjects(scene).front().object;
            using Transform = lux::simulation::ecs::Transform3D;
            const auto field = [](auto& value) { return &value.translation; };
            const auto* value =
                static_cast<const Transform*>(toolTest(scene).component(object, lux::cxx::typeToken<Transform>()));
            const Eigen::Vector3d before = value->translation;
            assert(toolTest(scene).setField<Transform>(
                *toolTest(scene).writeTarget(object),
                "translation",
                "Translation",
                field,
                Eigen::Vector3d{before.x() + 2, before.y(), before.z()}
            ));
            assert(scene.undo() && value->translation == before);
            assert(scene.redo() && value->translation.x() == before.x() + 2);
            assert(notices == 3);
            notification.disconnect();
            paused_translation = value->translation;
            stale_pause_target = *toolTest(scene).writeTarget(object);
            assert(toolTest(scene).select(object));
            assert(scene.reviewClose()->current == author.current);
            if (auto* editor = dynamic_cast<Editor*>(&scene.root()))
                EditorTestAccess::captureMenu(*editor, scene);
            paused_step = run.steps;
            frames = 0;
            phase = 2;
        }
        else if (phase == 2)
        {
            assert(run.state == scene::EPlaybackState::PAUSED && run.steps == paused_step);
            if (++frames < 8)
            {
                return false;
            }
            const auto* derived = static_cast<const lux::simulation::ecs::WorldTransform3D*>(toolTest(scene).component(
                stale_pause_target.entity,
                lux::cxx::typeToken<lux::simulation::ecs::WorldTransform3D>()
            ));
            assert(derived && derived->value.translation().isApprox(paused_translation, 1e-10));
            assert(scene.reviewClose()->revision == author.revision);
            assert(scene.stepRun(first));
            const auto duplicate = scene.stepRun(first);
            assert(!duplicate && duplicate.error().code == EEditorError::BUSY);
            phase = 3;
        }
        else if (phase == 3 && run.steps == paused_step + 1 && run.state == scene::EPlaybackState::PAUSED)
        {
            assert(scene.historyId() != pause_history && scene.historyId() != author.current.history);
            if (auto* editor = dynamic_cast<Editor*>(&scene.root()))
            {
                assert(!EditorTestAccess::validMenu(*editor));
                assert(
                    EditorTestAccess::executeMenu(*editor, editing::EHistoryAction::UNDO) ==
                    lux::ui::ECommandDispatchResult::NOT_FOUND
                );
            }
            assert(scene.historyView()->history.entry_count == 0);
            assert(scene.historyView()->undo == editing::EHistoryActionAvailability::EMPTY);
            const auto late = toolTest(scene).setField<lux::simulation::ecs::Transform3D>(
                stale_pause_target,
                "translation",
                "Translation",
                [](auto& value) { return &value.translation; },
                author_translation
            );
            assert(!late && late.error().code == editing::EEditError::WRONG_HISTORY);
            frames = 0;
            phase = 4;
        }
        else if (phase == 4)
        {
            assert(run.steps == paused_step + 1 && run.state == scene::EPlaybackState::PAUSED);
            if (++frames < 8)
            {
                return false;
            }
            assert(run.elapsed == std::chrono::milliseconds(10) * run.steps);
            const auto object = sceneObjects(scene).front().object;
            auto structural = toolTest(scene).eraseObjects(author.current, std::span(&object, 1));
            assert(!structural); // Runtime structural editing is outside this field-only pause scope.
            assert(run.captured_state == captured_state && run.retained_resources == 3);
            assert(scene.resumeRun(first));
            scene.content()->setVisible(false);
            const auto closeViewport = [&](auto&& self, lux::object::LuxObject& node) -> void {
                if (auto* viewport = dynamic_cast<lux::editor::views::ViewportElement*>(&node))
                    static_cast<void>(viewport->close());
                for (auto* child = node.firstChild(); child; child = child->nextSibling())
                    self(self, *child);
            };
            closeViewport(closeViewport, *scene.content());
            observer->requestClose();
            phase = 22;
        }
        else if (phase == 22)
        {
            if (observer->closeStatus().state != ECloseState::CLOSED)
                return false;
            assert(run.state == scene::EPlaybackState::RUNNING && run.retained_resources == 3);
            paused_step = run.steps;
            phase = 5;
            std::puts(
                "D06: lux::editor::views::ViewportElement closed; independent Run remains active with its resource uses"
            );
        }
        else if (phase == 5 && run.steps >= paused_step + 4)
        {
            observer->requestClose();
            assert(scene.stopRun(first));
            phase = 6;
        }
        else if (phase == 6 && run.state == scene::EPlaybackState::FINISHED)
        {
            assert(run.retained_resources == 0 && run.pending_updates == 0);
            assert(toolTest(scene).selection().object == selected);
            const auto* restored = static_cast<const lux::simulation::ecs::Transform3D*>(
                toolTest(scene).component(selected, lux::cxx::typeToken<lux::simulation::ecs::Transform3D>())
            );
            assert(restored && restored->translation == author_translation);
            assert(run.published_updates == run.forwarded_updates + run.retired_updates && run.update_high_water <= 1);
            std::printf(
                "Run transport: published=%llu forwarded=%llu high_water=%u work_us=%lld wait_us=%lld resources=%zu "
                "steps=%llu frames=%llu\n",
                static_cast<unsigned long long>(run.published_updates),
                static_cast<unsigned long long>(run.forwarded_updates),
                run.update_high_water,
                static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::microseconds>(run.simulation_work).count()
                ),
                static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::microseconds>(run.publication_wait).count()
                ),
                run.retained_resources,
                static_cast<unsigned long long>(run.steps),
                static_cast<unsigned long long>(renderer.statistics().frames - first_frame)
            );
            const auto next = scene.play(std::chrono::milliseconds(10));
            assert(next && *next != first_request);
            const auto late = scene.stopRun(first);
            assert(!late && late.error().code == EEditorError::STALE_REQUEST);
            phase = 7;
        }
        else if (phase == 7 && run.state == scene::EPlaybackState::RUNNING && run.steps >= 2)
        {
            assert(run.id != first);
            assert(scene.stopRun(run.id));
            phase = 8;
        }
        else if (phase == 8 && run.state == scene::EPlaybackState::FINISHED)
        {
            const auto cancelled = scene.play(std::chrono::milliseconds(10));
            assert(cancelled);
            assert(scene.cancelRun(*cancelled) && scene.cancelRun(*cancelled));
            phase = 9;
        }
        else if (phase == 9 && run.state == scene::EPlaybackState::FINISHED)
        {
            assert(run.retained_resources == 0 && run.steps == 0);
            const auto exit_run = scene.play(std::chrono::milliseconds(10));
            assert(exit_run);
            phase = 10;
        }
        else if (phase == 10 && run.state == scene::EPlaybackState::RUNNING && run.steps >= 2)
        {
            observer->requestClose();
            if (observer->closeStatus().state != ECloseState::CLOSED)
                return false;
            observer.reset();
            runtime = {};
            std::printf(
                "PASS fixed Run: GPU-completed preview, pause, exactly one step, resume, Stop, "
                "new identity restart, stale Stop rejected; Run leaves author history unchanged; explicit "
                "pause history epochs stay isolated; preparing Stop completed; active Run handed to normal "
                "window close, steps=%llu\n",
                static_cast<unsigned long long>(run.steps)
            );
            return true;
        }
        return false;
    }
};

struct CpuRunChecks final
{
    lux::editor::scene::RunId id;
    lux::editor::editing::HistorySnapshot author;
    std::optional<lux::editor::sessions::PersistedState> author_persisted;
    std::uint64_t paused{};
    unsigned phase{};

    void begin(lux::editor::scene::SceneEditor& scene)
    {
        author = scene.historyView()->history;
        author_persisted = scene.persistedState();
        auto started = scene.play(std::chrono::milliseconds(10));
        assert(started);
        assert(!scene.runStatus().id.valid());
    }

    bool poll(lux::editor::scene::SceneEditor& scene)
    {
        using namespace lux::editor::scene;
        const auto status = scene.runStatus();
        if (!id.valid() && status.id.valid())
            id = status.id;
        assert(status.result && !status.render_scene.isValid());
        if (phase == 0 && status.steps >= 2)
        {
            assert(scene.pauseRun(id));
            phase = 1;
        }
        else if (phase == 1 && status.state == EPlaybackState::PAUSED)
        {
            paused = status.steps;
            assert(scene.stepRun(id));
            phase = 2;
        }
        else if (phase == 2 && status.state == EPlaybackState::PAUSED)
        {
            assert(status.steps == paused + 1);
            assert(scene.stopRun(id));
            phase = 3;
        }
        else if (phase == 3 && status.state == EPlaybackState::FINISHED)
        {
            const auto history = scene.historyView()->history;
            assert(history.current == author.current && history.revision == author.revision);
            assert(status.retained_resources == 0);
            std::puts("PASS Scene without RenderSystem: Main Play/Pause/Step/Stop, author history preserved; GUI "
                      "remains available");
            return true;
        }
        return false;
    }
};

struct RunFailureChecks final
{
    lux::editor::scene::RunId failed_run;
    lux::editor::editing::HistorySnapshot author;
    std::optional<lux::editor::sessions::PersistedState> author_persisted;
    lux::simulation::ecs::Entity object{lux::simulation::ecs::NullEntity};
    unsigned phase{10};
    std::uint64_t before_failed_step{};
    Eigen::Vector3d author_translation;

    void begin(lux::editor::scene::SceneEditor& scene)
    {
        using Transform = lux::simulation::ecs::Transform3D;
        object = sceneObjects(scene).front().object;
        author_translation =
            static_cast<const Transform*>(toolTest(scene).component(object, lux::cxx::typeToken<Transform>()))
                ->translation;
        author = scene.historyView()->history;
        author_persisted = scene.persistedState();
        auto started = scene.play(std::chrono::milliseconds(10));
        assert(started);
        assert(!scene.runStatus().id.valid());
    }
    bool poll(lux::editor::scene::SceneEditor& scene)
    {
        using namespace lux;
        using namespace lux::editor;
        const auto run = scene.runStatus();
        if (!failed_run.valid() && run.id.valid())
            failed_run = run.id;
        if (phase == 10 && run.state == lux::editor::scene::EPlaybackState::RUNNING && run.steps >= 2)
        {
            assert(scene.pauseRun(run.id));
            phase = 11;
            return false;
        }
        if (phase == 11 && run.state == lux::editor::scene::EPlaybackState::PAUSED)
        {
            using Transform = simulation::ecs::Transform3D;
            const auto runtime_object = sceneObjects(scene).front().object;
            // The failure belongs to the independent runtime instance. Leave
            // author data valid so its own resumed Driver has no separate fault.
            assert(toolTest(scene).setField<Transform>(
                *toolTest(scene).writeTarget(runtime_object),
                "Transform3D.translation",
                "Translation",
                [](auto& value) { return &value.translation; },
                Eigen::Vector3d{1.e100, 1, 0}
            ));
            before_failed_step = run.steps;
            assert(scene.resumeRun(run.id));
            phase = 0;
            return false;
        }
        if (phase == 0)
        {
            if (run.state != lux::editor::scene::EPlaybackState::FAILED)
            {
                return false;
            }
            assert(!run.result && run.result.error().domain == "run.drive");
            const auto* cause = std::any_cast<lux::scene::SceneExecutionFailure>(&run.result.error().cause);
            assert(
                cause && cause->code == lux::scene::ESceneExecutionError::SYSTEM_FAILURE &&
                cause->system == lux::system::SystemInstanceId{2}
            );
            const auto current = scene.historyView()->history;
            assert(
                current.current == author.current && scene.persistedState() == author_persisted &&
                current.revision == author.revision && current.cursor == author.cursor
            );
            const auto* value = static_cast<const simulation::ecs::Transform3D*>(
                toolTest(scene).component(object, cxx::typeToken<simulation::ecs::Transform3D>())
            );
            assert(value && value->translation == author_translation);
            assert(run.retained_resources == 0 && run.pending_updates == 0);
            std::printf(
                "JR02 render publication failure final steps=%llu elapsed_ns=%lld "
                "work_ns=%lld\n",
                static_cast<unsigned long long>(run.steps),
                static_cast<long long>(run.elapsed.count()),
                static_cast<long long>(run.simulation_work.count())
            );
            assert(
                run.steps == before_failed_step && run.elapsed == std::chrono::milliseconds(10) * run.steps &&
                "Paused-edit publication failure must retain the last adopted Simulation time"
            );
            assert(
                run.failed_phase == lux::editor::scene::ERunPhase::PUBLICATION &&
                run.completed.simulation == run.steps && run.completed.stable == run.steps &&
                run.completed.publication == before_failed_step
            );
            assert(run.simulation_work.count() > 0);
            std::printf(
                "D04/D08 exact Run failure: domain=%s reason=%llu system=2 author preserved pins=0 pending=0\n",
                run.result.error().domain.c_str(),
                static_cast<unsigned long long>(run.result.error().reason)
            );
            assert(scene.historyView()->history.current == author.current);
            auto restarted = scene.play(std::chrono::milliseconds(10));
            assert(restarted && !scene.runStatus().id.valid());
            phase = 1;
        }
        else if (phase == 1 && run.state == lux::editor::scene::EPlaybackState::RUNNING && run.steps >= 2)
        {
            assert(run.result && run.id != failed_run);
            assert(scene.stopRun(run.id));
            phase = 2;
        }
        else if (phase == 2 && run.state == lux::editor::scene::EPlaybackState::FINISHED)
        {
            assert(run.result && run.retained_resources == 0);
            const auto current = scene.historyView()->history;
            assert(current.current == author.current && current.revision == author.revision);
            std::puts("PASS Run failure preserved author; unchanged author launched a new independent Run and closed");
            return true;
        }
        return false;
    }
};

#pragma once

#include <cassert>
#include <cstdio>
#include <lux/engine/editor/gui/GuiView.hpp>
#include <lux/engine/editor/rendering/EditorRenderer.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/scene/Scene.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>

// Observe the frontend's real image collection, before seal releases Pane frame leases.
// Polling Pane images after seal would always see an empty list.
class RunImageProbe final : public lux::object::Object<RunImageProbe, lux::ui::Pane>, public lux::editor::gui::GuiView
{
  public:
    RunImageProbe(lux::editor::scene::SceneEditor &scene, lux::editor::rendering::ViewImage &captured)
        : Object(scene.dispatcherRef(), lux::ui::PaneId{"test.run-image"}, lux::ui::PaneTypeId{"test.observer"},
                 "Run image observer"),
          scene_(scene), captured_(captured)
    {
        setVisible(false);
    }
    std::string_view id() const noexcept override
    {
        return "test.run-image";
    }
    lux::ui::Pane &pane() noexcept override
    {
        return *this;
    }
    void requestClose() noexcept override
    {
        captured_ = {};
        closed_ = true;
    }
    void poll(lux::editor::PollBudget &) override {}
    lux::editor::CloseStatus closeStatus() const override
    {
        return {closed_ ? lux::editor::ECloseState::CLOSED : lux::editor::ECloseState::OPEN, {}};
    }
    void appendFrameImages(std::vector<lux::editor::rendering::ViewImage> &) const override
    {
        if (closed_ || captured_.lease.valid())
        {
            return;
        }
        for (const auto &view : scene_.views())
        {
            const bool is_run = scene_.runStatus().state == lux::editor::scene::ERunState::RUNNING;
            if (!view->id().ends_with("-view"))
            {
                continue;
            }
            auto *gui = dynamic_cast<lux::editor::gui::GuiView *>(view.get());
            assert(gui);
            std::vector<lux::editor::rendering::ViewImage> images;
            gui->appendFrameImages(images);
            if (!images.empty())
            {
                // Reproduce closing a Pane in the same frame as its image draw.
                // This is an interface regression, not physical mouse evidence.
                const bool visible = gui->pane().visible();
                gui->pane().setVisible(false);
                std::vector<lux::editor::rendering::ViewImage> hidden_images;
                gui->appendFrameImages(hidden_images);
                gui->pane().setVisible(visible);
                assert(hidden_images.size() == images.size());
                assert(hidden_images.front().texture == images.front().texture);
                if (!is_run)
                {
                    scene_hidden_checked_ = true;
                }
                if (is_run)
                {
                    captured_ = images.front();
                    std::puts("PASS Run/Scene draw image references survive same-frame Pane hide");
                }
            }
        }
    }

  private:
    void draw(lux::ui::Frame &, lux::ui::PaneDrawContext &) override {}
    lux::editor::scene::SceneEditor &scene_;
    lux::editor::rendering::ViewImage &captured_;
    mutable bool scene_hidden_checked_{};
    bool closed_{};
};

struct SceneRunChecks final
{
    lux::editor::scene::RunId first;
    lux::editor::editing::HistorySnapshot author;
    lux::editor::editing::HistoryId pause_history;
    std::uint64_t paused_step{};
    std::uint64_t first_frame{};
    std::size_t phase{}, frames{};
    std::size_t polls{};
    lux::editor::rendering::ViewImage held_image;
    lux::scene::RenderRuntimeLease runtime;
    lux::render::MeshStackOperationIds mesh_ops;
    lux::render::RenderRequest<lux::render::MeshStackStatsReply> run_mesh, author_mesh;
    bool initial_adopted{};
    lux::world::WorldObjectId selected;
    Eigen::Vector3d author_translation, paused_translation;
    lux::editor::scene::SceneWriteTarget stale_pause_target;
    lux::editor::editing::StateId captured_state;

    void query(lux::editor::scene::SceneEditor &scene)
    {
        lux::render::MeshStackControlClient client(runtime.control(), mesh_ops);
        run_mesh = client.stats({scene.runStatus().render_scene});
        author_mesh = client.stats({*scene.renderScene()});
    }

    void begin(lux::editor::scene::SceneEditor &scene, lux::editor::rendering::EditorRenderer &renderer)
    {
        selected = scene.objects().front().object;
        assert(scene.select(selected));
        author_translation = static_cast<const lux::simulation::ecs::Transform3D *>(
                                 scene.component(selected, lux::cxx::typeToken<lux::simulation::ecs::Transform3D>()))
                                 ->translation;
        author = scene.historyView()->history;
        first_frame = renderer.statistics().frames;
        captured_state = author.current;
        auto acquired = renderer.acquire();
        assert(acquired);
        runtime = std::move(*acquired);
        mesh_ops = runtime.features().ops<lux::render::MeshStackOperationIds>("StandardMeshStack");
        assert(mesh_ops.valid());
        std::vector<std::unique_ptr<lux::editor::DocumentView>> observers;
        observers.push_back(std::make_unique<RunImageProbe>(scene, held_image));
        assert(scene.addViews(observers));
        const auto invalid_delta = scene.play(std::chrono::nanoseconds(0));
        assert(!invalid_delta && invalid_delta.error().code == lux::editor::EEditorError::INVALID_ARGUMENT);
        auto started = scene.play(std::chrono::milliseconds(10));
        assert(started);
        first = *started;
        const auto duplicate = scene.play();
        assert(!duplicate && duplicate.error().code == lux::editor::EEditorError::BUSY);
    }

    bool poll(lux::editor::scene::SceneEditor &scene, lux::editor::rendering::EditorRenderer &renderer)
    {
        using namespace lux::editor;
        const auto run = scene.runStatus();
        if (++polls % 120 == 0)
        {
            std::printf("Run trace phase=%zu state=%u steps=%llu render_views=%zu held=%d\n", phase,
                        unsigned(run.state), static_cast<unsigned long long>(run.steps), renderer.statistics().views,
                        held_image.lease.valid());
        }
        if (!run.result)
        {
            std::fprintf(stderr, "Run failure: %s reason=%llu %s\n", run.result.error().domain.c_str(),
                         static_cast<unsigned long long>(run.result.error().reason),
                         run.result.error().message.c_str());
        }
        assert(run.result);
        const auto current = scene.historyView()->history;
        if (current.current.history == author.current.history)
        {
            assert(current.current == author.current && current.saved == author.saved &&
                   current.revision == author.revision && current.cursor == author.cursor);
        }
        if (phase == 0 && run.state == scene::ERunState::RUNNING && run.steps >= 3)
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
                assert(run.render_scene != *scene.renderScene() && run.retained_resources == 3);
                initial_adopted = true;
                held_image = {}; // Observe a frame after the Run's content was adopted.
                return false;
            }
            bool completed{};
            if (held_image.lease.valid())
            {
                auto evidence = renderer.imageEvidence(held_image);
                completed = evidence && evidence->evidence == rendering::EImageEvidence::GPU_COMPLETE;
            }
            if (!completed)
            {
                return false;
            }
            assert(scene.pauseRun(first));
            phase = 1;
        }
        else if (phase == 1 && run.state == scene::ERunState::PAUSED)
        {
            pause_history = scene.historyId();
            assert(pause_history != author.current.history);
            const auto object = scene.objects().front().object;
            using Transform = lux::simulation::ecs::Transform3D;
            const auto field = [](auto &value) { return &value.translation; };
            const auto *value =
                static_cast<const Transform *>(scene.component(object, lux::cxx::typeToken<Transform>()));
            const Eigen::Vector3d before = value->translation;
            assert(scene.setField<Transform>(*scene.writeTarget(object), "translation", "Translation", field,
                                             Eigen::Vector3d{before.x() + 2, before.y(), before.z()}));
            assert(scene.undo() && value->translation == before);
            assert(scene.redo() && value->translation.x() == before.x() + 2);
            paused_translation = value->translation;
            stale_pause_target = *scene.writeTarget(object);
            assert(scene.select(object));
            assert(scene.reviewClose()->current == author.current);
            paused_step = run.steps;
            frames = 0;
            phase = 2;
        }
        else if (phase == 2)
        {
            assert(run.state == scene::ERunState::PAUSED && run.steps == paused_step);
            if (++frames < 8)
            {
                return false;
            }
            const auto *derived = static_cast<const lux::simulation::ecs::WorldTransform3D *>(
                scene.component(selected, lux::cxx::typeToken<lux::simulation::ecs::WorldTransform3D>()));
            assert(derived && derived->value.translation().isApprox(paused_translation, 1e-10));
            assert(scene.reviewClose()->revision == author.revision);
            assert(scene.stepRun(first));
            const auto duplicate = scene.stepRun(first);
            assert(!duplicate && duplicate.error().code == EEditorError::BUSY);
            phase = 3;
        }
        else if (phase == 3 && run.steps == paused_step + 1 && run.state == scene::ERunState::PAUSED)
        {
            assert(scene.historyId() != pause_history && scene.historyId() != author.current.history);
            assert(scene.historyView()->history.entry_count == 0);
            assert(scene.historyView()->undo == editing::EHistoryActionAvailability::EMPTY);
            const auto late = scene.setField<lux::simulation::ecs::Transform3D>(
                stale_pause_target, "translation", "Translation", [](auto &value) { return &value.translation; },
                author_translation);
            assert(!late && late.error().code == editing::EEditError::WRONG_HISTORY);
            frames = 0;
            phase = 4;
        }
        else if (phase == 4)
        {
            assert(run.steps == paused_step + 1 && run.state == scene::ERunState::PAUSED);
            if (++frames < 8)
            {
                return false;
            }
            assert(run.elapsed == std::chrono::milliseconds(10) * run.steps);
            const auto object = scene.objects().front().object;
            auto structural = scene.eraseObjects(author.current, std::span(&object, 1));
            assert(!structural); // Runtime structural editing is outside this field-only pause scope.
            assert(run.captured_state == captured_state && run.retained_resources == 3);
            assert(scene.resumeRun(first));
            for (const auto &pane : scene.views())
            {
                if (pane->id().ends_with("-view") || pane->id() == "test.run-image")
                {
                    pane->requestClose();
                }
            }
            phase = 22;
        }
        else if (phase == 22)
        {
            for (const auto &view : scene.views())
            {
                if (view->id().ends_with("-view"))
                {
                    return false;
                }
            }
            assert(run.state == scene::ERunState::RUNNING && run.retained_resources == 3);
            paused_step = run.steps;
            phase = 5;
            std::puts("D06: ScenePane destroyed; independent Run remains active with its resource pins");
        }
        else if (phase == 5 && run.steps >= paused_step + 4)
        {
            for (const auto &view : scene.views())
            {
                if (view->id() == "test.run-image")
                {
                    view->requestClose();
                }
            }
            assert(scene.stopRun(first));
            phase = 6;
        }
        else if (phase == 6 && run.state == scene::ERunState::FINISHED)
        {
            assert(run.retained_resources == 0 && run.pending_updates == 0);
            assert(scene.selection().object == selected);
            const auto *restored = static_cast<const lux::simulation::ecs::Transform3D *>(
                scene.component(selected, lux::cxx::typeToken<lux::simulation::ecs::Transform3D>()));
            assert(restored && restored->translation == author_translation);
            assert(run.published_updates == run.forwarded_updates && run.update_high_water <= 1);
            std::printf("Run transport: published=%llu forwarded=%llu high_water=%u work_us=%lld wait_us=%lld pins=%zu "
                        "steps=%llu frames=%llu\n",
                        static_cast<unsigned long long>(run.published_updates),
                        static_cast<unsigned long long>(run.forwarded_updates), run.update_high_water,
                        static_cast<long long>(
                            std::chrono::duration_cast<std::chrono::microseconds>(run.simulation_work).count()),
                        static_cast<long long>(
                            std::chrono::duration_cast<std::chrono::microseconds>(run.publication_wait).count()),
                        run.retained_resources, static_cast<unsigned long long>(run.steps),
                        static_cast<unsigned long long>(renderer.statistics().frames - first_frame));
            const auto next = scene.play(std::chrono::milliseconds(10));
            assert(next && *next != first);
            const auto late = scene.stopRun(first);
            assert(!late && late.error().code == EEditorError::STALE_REQUEST);
            phase = 7;
        }
        else if (phase == 7 && run.state == scene::ERunState::RUNNING && run.steps >= 2)
        {
            assert(scene.stopRun(run.id));
            phase = 8;
        }
        else if (phase == 8 && run.state == scene::ERunState::FINISHED)
        {
            const auto cancelled = scene.play(std::chrono::milliseconds(10));
            assert(cancelled);
            assert(scene.stopRun(*cancelled) && scene.stopRun(*cancelled));
            phase = 9;
        }
        else if (phase == 9 && run.state == scene::ERunState::FINISHED)
        {
            assert(run.retained_resources == 0 && run.steps == 0);
            const auto exit_run = scene.play(std::chrono::milliseconds(10));
            assert(exit_run);
            phase = 10;
        }
        else if (phase == 10 && run.state == scene::ERunState::RUNNING && run.steps >= 2)
        {
            runtime = {};
            std::printf("PASS fixed Run: GPU-completed preview, pause, exactly one step, resume, Stop, "
                        "new identity restart, stale Stop rejected; Run leaves author history unchanged; explicit "
                        "pause history epochs stay isolated; preparing Stop completed; active Run handed to normal "
                        "window close, steps=%llu\n",
                        static_cast<unsigned long long>(run.steps));
            return true;
        }
        return false;
    }
};

struct CpuRunChecks final
{
    lux::editor::scene::RunId id;
    lux::editor::editing::HistorySnapshot author;
    std::uint64_t paused{};
    unsigned phase{};

    void begin(lux::editor::scene::SceneEditor &scene)
    {
        author = scene.historyView()->history;
        auto started = scene.play(std::chrono::milliseconds(10));
        assert(started);
        id = *started;
    }

    bool poll(lux::editor::scene::SceneEditor &scene)
    {
        using namespace lux::editor::scene;
        const auto status = scene.runStatus();
        assert(status.result && !status.render_scene.isValid());
        if (phase == 0 && status.steps >= 2)
        {
            assert(scene.pauseRun(id));
            phase = 1;
        }
        else if (phase == 1 && status.state == ERunState::PAUSED)
        {
            paused = status.steps;
            assert(scene.stepRun(id));
            phase = 2;
        }
        else if (phase == 2 && status.state == ERunState::PAUSED)
        {
            assert(status.steps == paused + 1);
            assert(scene.stopRun(id));
            phase = 3;
        }
        else if (phase == 3 && status.state == ERunState::FINISHED)
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
    lux::world::WorldObjectId object;
    unsigned phase{};

    void begin(lux::editor::scene::SceneEditor &scene)
    {
        using Transform = lux::simulation::ecs::Transform3D;
        object = scene.objects().front().object;
        const auto field = [](auto &value) noexcept { return &value.translation; };
        // Finite author data, but outside the Render protocol's page-delta range.
        assert(scene.setField<Transform>(*scene.writeTarget(object), "Transform3D.translation", "Translation", field,
                                         Eigen::Vector3d{1.e100, 1, 0}));
        author = scene.historyView()->history;
        auto started = scene.play(std::chrono::milliseconds(10));
        assert(started);
        failed_run = *started;
    }
    bool poll(lux::editor::scene::SceneEditor &scene)
    {
        using namespace lux;
        using namespace lux::editor;
        const auto run = scene.runStatus();
        if (phase == 0)
        {
            if (run.state != lux::editor::scene::ERunState::FAILED)
            {
                return false;
            }
            assert(!run.result && run.result.error().domain == "run.stable");
            const auto *cause = std::any_cast<lux::scene::SceneExecutionFailure>(&run.result.error().cause);
            assert(cause && cause->code == lux::scene::ESceneExecutionError::SYSTEM_FAILURE &&
                   cause->system == lux::system::SystemInstanceId{2});
            const auto current = scene.historyView()->history;
            assert(current.current == author.current && current.saved == author.saved &&
                   current.revision == author.revision && current.cursor == author.cursor);
            const auto *value = static_cast<const simulation::ecs::Transform3D *>(
                scene.component(object, cxx::typeToken<simulation::ecs::Transform3D>()));
            assert(value && value->translation.x() == 1.e100);
            assert(run.retained_resources == 0 && run.pending_updates == 0);
            std::printf("JR02 stable failure final steps=%llu elapsed_ns=%lld "
                        "work_ns=%lld\n",
                        static_cast<unsigned long long>(run.steps), static_cast<long long>(run.elapsed.count()),
                        static_cast<long long>(run.simulation_work.count()));
            assert(run.steps == 1 && run.elapsed == std::chrono::milliseconds(10) &&
                   "JR02 stable failure must retain the adopted Simulation clock");
            assert(run.failed_phase == lux::editor::scene::ERunPhase::STABLE && run.completed.simulation == 1 &&
                   run.completed.stable == 0 && run.completed.publication == 0);
            assert(run.simulation_work.count() > 0);
            std::printf("D04/D08 exact Run failure: domain=%s reason=%llu system=2 author preserved pins=0 pending=0\n",
                        run.result.error().domain.c_str(), static_cast<unsigned long long>(run.result.error().reason));
            assert(scene.undo());
            author = scene.historyView()->history;
            auto restarted = scene.play(std::chrono::milliseconds(10));
            assert(restarted && *restarted != failed_run);
            phase = 1;
        }
        else if (phase == 1 && run.state == lux::editor::scene::ERunState::RUNNING && run.steps >= 2)
        {
            assert(run.result);
            assert(scene.stopRun(run.id));
            phase = 2;
        }
        else if (phase == 2 && run.state == lux::editor::scene::ERunState::FINISHED)
        {
            assert(run.result && run.retained_resources == 0);
            const auto current = scene.historyView()->history;
            assert(current.current == author.current && current.revision == author.revision);
            std::puts("PASS Run failure preserved author; corrected author launched a new independent Run and closed");
            return true;
        }
        return false;
    }
};

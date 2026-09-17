#include "TestExit.hpp"
#include "flow_metadata.hpp"
#include "model_placement_checks.hpp"
#include "render_association_checks.hpp"
#include "render_thread_checks.hpp"
#include "run_checks.hpp"
#include "scene_edit_checks.hpp"
#include "scene_save_checks.hpp"
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <lux/engine/editor/Editor.hpp>
#include <lux/engine/editor/flowforge/FlowForgeEditor.hpp>
#include <lux/engine/editor/gui/GuiFrontend.hpp>
#include <lux/engine/editor/gui/GuiView.hpp>
#include <lux/engine/editor/gui/actions/HistoryActions.hpp>
#include <lux/engine/editor/gui/flowforge/FlowForgeDocumentProvider.hpp>
#include <lux/engine/editor/gui/material/MaterialDocumentProvider.hpp>
#include <lux/engine/editor/gui/scene/SceneDocumentProvider.hpp>
#include <lux/engine/editor/material/MaterialEditor.hpp>
#include <lux/engine/editor/project/Project.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/object/detail/MessageEnvelope.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/scene/Scene.hpp>
#include <lux/engine/scene/WorldMaterializer.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/engine/ui/CommandRouter.hpp>
#include <lux/engine/ui/Pane.hpp>

using namespace lux::editor;

struct Evidence final
{
    rendering::EditorRenderer *renderer{};
    gui::EditorWindow *window{};
    std::size_t checks{}, frames{};
    std::size_t expected_objects{4}, cost_draws{};
    std::chrono::nanoseconds cost_draw_time{};
    std::size_t cost_retries{};
    std::chrono::nanoseconds cost_retry_time{};
    bool closed{}, rollback{};
    std::string mode{"basic"};
    std::atomic_bool failed{};
    std::chrono::nanoseconds callbacks{}, waits{};
};

struct SampleTime final
{
    std::chrono::nanoseconds &elapsed;
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
    ~SampleTime()
    {
        elapsed += std::chrono::steady_clock::now() - start;
    }
};

void checkLocalHistory(lux::editor::scene::SceneEditor &scene, DocumentEditor &other)
{
    const auto before = other.historyView()->history;
    const auto scene_before = scene.historyView()->history;
    assert(scene.historyView()->undo == editing::EHistoryActionAvailability::EMPTY);
    assert(other.historyView()->undo == editing::EHistoryActionAvailability::READY);
    gui::HistoryActions local(scene.dispatcherRef(), scene);
    gui::HistoryActions remote(scene.dispatcherRef(), other);
    std::size_t failures{};
    auto connection = local.observeScoped<gui::HistoryActions::failed>(
        [&](const gui::HistoryActionFailure &failure) noexcept
        {
            assert(failure.target == scene.historyId() && failure.failure.code == editing::EEditError::NO_UNDO);
            ++failures;
        });
    lux::ui::CommandRouter router;
    const auto a = router.defineCommand({lux::ui::UiCommandId{"test.scene.undo"}, "Scene Undo"});
    const auto b = router.defineCommand({lux::ui::UiCommandId{"test.other.undo"}, "Other Undo"});
    assert(a && b);
    auto bind_a = router.bindGlobal<&gui::HistoryActions::undo, &gui::HistoryActions::canUndo>(*a, local);
    auto bind_b = router.bindGlobal<&gui::HistoryActions::undo, &gui::HistoryActions::canUndo>(*b, remote);
    assert(bind_a && bind_b && !router.state(*a).enabled && router.state(*b).enabled);
    assert(router.invoke(*a) == lux::ui::ECommandDispatchResult::DISABLED && failures == 0);
    local.undo();
    assert(failures == 1 && other.historyView()->history.current == before.current);
    assert(other.historyView()->history.cursor == before.cursor &&
           other.historyView()->history.revision == before.revision);
    assert(router.invoke(*b) == lux::ui::ECommandDispatchResult::EXECUTED);
    assert(other.redo() && other.historyView()->history.current == before.current);
    assert(scene.historyView()->history.current == scene_before.current &&
           scene.historyView()->history.revision == scene_before.revision);
    std::puts("PASS real CommandRouter: disabled local Undo never dispatched; entered empty action emitted NO_UNDO; "
              "other real document unchanged; explicit other binding executed");
}

void checkThreeHistories(lux::editor::scene::SceneEditor &scene, material::MaterialEditor &a,
                         material::MaterialEditor &b)
{
    using Transform = lux::simulation::ecs::Transform3D;
    const auto object = scene.objects().front().object;
    const auto field = [](auto &value) noexcept { return &value.translation; };
    const auto original =
        static_cast<const Transform *>(scene.component(object, lux::cxx::typeToken<Transform>()))->translation;
    const Eigen::Vector3d edited = original + Eigen::Vector3d{3, 0, 0};
    assert(
        scene.setField<Transform>(*scene.writeTarget(object), "Transform3D.translation", "Translation", field, edited));
    assert(a.rename("A local history") && b.rename("B must stay unchanged"));
    const auto scene_before = scene.historyView()->history;
    const auto b_before = b.historyView()->history;
    std::size_t scene_notices{}, b_notices{}, failures{};
    auto scene_connection =
        scene.observeScoped<scene::SceneEditor::componentChanged>([&](const auto &) noexcept { ++scene_notices; });
    auto b_connection =
        b.observeScoped<material::MaterialEditor::contentChanged>([&](const auto &) noexcept { ++b_notices; });
    gui::HistoryActions action(a.dispatcherRef(), a);
    auto failure_connection = action.observeScoped<gui::HistoryActions::failed>(
        [&](const gui::HistoryActionFailure &failure) noexcept
        {
            assert(failure.target == a.historyId() && failure.failure.code == editing::EEditError::NO_UNDO);
            ++failures;
        });
    lux::ui::CommandRouter commands;
    const auto command = commands.defineCommand({lux::ui::UiCommandId{"three.a.undo"}, "A Undo"});
    assert(command);
    auto binding = commands.bindGlobal<&gui::HistoryActions::undo, &gui::HistoryActions::canUndo>(*command, action);
    assert(binding && commands.invoke(*command) == lux::ui::ECommandDispatchResult::EXECUTED);
    assert(a.historyView()->undo == editing::EHistoryActionAvailability::EMPTY);
    assert(commands.invoke(*command) == lux::ui::ECommandDispatchResult::DISABLED);
    action.undo();
    assert(failures == 1 && b_notices == 0 && scene_notices == 0);
    assert(b.historyView()->history.current == b_before.current &&
           b.historyView()->history.revision == b_before.revision);
    assert(scene.historyView()->history.current == scene_before.current &&
           scene.historyView()->history.revision == scene_before.revision);

    assert(a.redo());
    auto histories = gui::ActiveEditHistory::create(3);
    assert(histories);
    auto registered_a = (*histories)->registerTarget(a);
    auto registered_b = (*histories)->registerTarget(b);
    auto registered_scene = (*histories)->registerTarget(scene);
    assert(registered_a && registered_b && registered_scene);
    gui::HistoryMenuActions menu(a.dispatcherRef(), **histories);
    assert((*histories)->activate(registered_a->handle()) && menu.capture());
    assert((*histories)->activate(registered_b->handle()));
    menu.undo();
    assert(a.historyView()->undo == editing::EHistoryActionAvailability::EMPTY);
    assert(b_notices == 0 && scene_notices == 0);
    std::size_t stale{};
    auto stale_connection = menu.observeScoped<gui::HistoryMenuActions::failed>(
        [&](const gui::HistoryActionFailure &failure) noexcept
        {
            assert(failure.target == a.historyId() && failure.failure.code == editing::EEditError::STALE_TARGET);
            ++stale;
        });
    assert(registered_a->reset());
    auto replacement = (*histories)->registerTarget(a);
    assert(replacement && (*histories)->activate(replacement->handle()));
    menu.undo();
    assert(stale == 1 && b_notices == 0 && scene_notices == 0);
    scene_connection.reset();
    b_connection.reset();
    assert(scene.undo() && b.undo());
    std::puts("PASS three real documents: A local/EMPTY Undo never changes B or Scene; captured menu stays on A "
              "after activation changes; retired registration rejected as STALE_TARGET after slot reuse");
}

class Probe final : public EditorFrontend
{
    TestExit exit_;

  public:
    Probe(Evidence &evidence, gui::GuiConfig config)
        : evidence_(evidence), inner_(gui::makeGuiFrontend(std::move(config)))
    {
    }
    EditorResult<void> beginStartup(Editor &editor, lux::process::ExecutionRuntime &runtime,
                                    lux::object::ObjectDispatcherRef dispatcher) override
    {
        editor_ = &editor;
        runtime_ = &runtime;
        auto started = inner_->beginStartup(editor, runtime, dispatcher);
        if (started && evidence_.mode == "background")
        {
            auto task = stdexec::then(stdexec::schedule(runtime.cpu()),
                                      [this]() noexcept
                                      {
                                          background_active_.store(true, std::memory_order_release);
                                          background_stop_.wait(false, std::memory_order_acquire);
                                          background_done_.store(true, std::memory_order_release);
                                      });
            auto errors = stdexec::upon_error(std::move(task),
                                              [this](lux::process::EExecutionError) noexcept
                                              {
                                                  evidence_.failed = true;
                                                  background_done_.store(true, std::memory_order_release);
                                              });
            auto stopped = stdexec::upon_stopped(std::move(errors),
                                                 [this]() noexcept
                                                 {
                                                     evidence_.failed = true;
                                                     background_done_.store(true, std::memory_order_release);
                                                 });
            const auto admitted = background_.start(std::move(stopped));
            assert(admitted);
        }
        if (evidence_.mode == "cancel-startup")
        {
            exit_.request(editor);
        }
        if (evidence_.mode == "invalid-window")
        {
            assert(!started && started.error().domain == "editor.window");
            assert(std::any_cast<gui::WindowFailure>(&started.error().cause)->code ==
                   gui::EWindowError::INVALID_ARGUMENT);
            ++evidence_.checks;
        }
        if (evidence_.mode == "invalid-renderer")
        {
            assert(!started && started.error().domain == "editor.renderer");
            assert(std::any_cast<rendering::RendererFailure>(&started.error().cause)->code ==
                   rendering::ERendererError::INVALID_ARGUMENT);
            ++evidence_.checks;
        }
        return started;
    }
    EditorResult<void> enterProject(Editor &editor, Project &project, lux::process::ExecutionRuntime &runtime,
                                    lux::object::ObjectDispatcherRef dispatcher) override
    {
        editor_ = &editor;
        project_ = &project;
        started_ = std::chrono::steady_clock::now();
        return inner_->enterProject(editor, project, runtime, dispatcher);
    }
    void collectInput(Editor &editor) override
    {
        SampleTime sample{evidence_.callbacks};
        inner_->collectInput(editor);
    }
    void poll(PollBudget &budget) override
    {
        exit_.poll();
        SampleTime sample{evidence_.callbacks};
        if (!closing_ && stage_ == 91 && evidence_.renderer &&
            evidence_.renderer->state() == rendering::ERendererState::READY)
        {
            // Match the normal document-before-presentation owner order. A
            // test packet offered only after GUI submission can indefinitely
            // lose every available Program slot to the next UI frame.
            auto &scene = dynamic_cast<lux::editor::scene::SceneEditor &>(editor_->document(handle_)->get());
            if (association_checks_.poll(scene))
            {
                exit_.request(*editor_);
                stage_ = 92;
            }
        }
        inner_->poll(budget);
        if (extra_view_ && evidence_.failed)
        {
            held_image_ = {};
            pending_packet_ = {};
            static_cast<void>(extra_view_->beginClose());
            const auto closed = extra_view_->advanceClose();
            if (closed && *closed == rendering::ERenderClose::COMPLETE)
            {
                extra_view_.reset();
            }
        }
        if (closing_ || !evidence_.renderer || !project_)
        {
            return;
        }
        if (std::chrono::steady_clock::now() - started_ > std::chrono::seconds(30))
        {
            if (!evidence_.failed)
            {
                std::fprintf(stderr, "FAIL deadline stage=%u\n", stage_);
                if (evidence_.mode == "render-association")
                {
                    const auto stats = evidence_.renderer->statistics();
                    std::fprintf(stderr, "association phase=%u pending=%u marker_retired=%u "
                                         "renderer=%u frames=%llu gpu=%llu events=%llu validation=%llu\n",
                                 association_checks_.phase, association_checks_.pending,
                                 association_checks_.consumed && association_checks_.consumed->load(),
                                 unsigned(evidence_.renderer->state()), stats.frames, stats.gpu_completed,
                                 stats.render_events, stats.validation_errors);
                }
            }
            evidence_.failed = true;
            exit_.request(*editor_);
            return;
        }
        if (evidence_.renderer->state() != rendering::ERendererState::READY)
        {
            return;
        }
        if (stage_ == 0)
        {
            const auto &asset = project_->manifest().assets.front();
            request_ = {{project_->manifest().id, asset.id, std::string(scene::kSceneDocumentType)}, "probe"};
            auto a = editor_->requestOpen(request_);
            auto b = editor_->requestOpen(request_);
            assert(a && b && *a != *b);
            first_ = *a;
            second_ = *b;
            assert(editor_->cancelOpen(first_));
            assert(std::holds_alternative<OpenCancelled>(*editor_->openStatus(first_)));
            assert(editor_->acknowledgeOpen(first_));
            assert(!editor_->openStatus(first_));
            ++stage_;
            evidence_.checks += 5;
        }
        if (stage_ == 1)
        {
            const auto status = editor_->openStatus(second_);
            assert(status);
            if (const auto *failure = std::get_if<EditorFailure>(&*status))
            {
                if (evidence_.mode == "bad-version")
                {
                    assert(failure->domain == "world.materialize");
                    const auto *original = std::any_cast<lux::scene::WorldMaterializeFailure>(&failure->cause);
                    assert(original && original->code == lux::scene::EWorldMaterializeError::COMPONENT_DECODE_FAILURE);
                    assert(original->component.code ==
                           lux::simulation::ecs::EComponentDecodeError::UNSUPPORTED_VERSION);
                    assert(editor_->documents().empty());
                    std::printf("expected version rejection: %s:%llu component=%u objects=0\n", failure->domain.c_str(),
                                failure->reason, unsigned(original->component.code));
                    evidence_.checks += 3;
                    exit_.request(*editor_);
                    return;
                }
                if (evidence_.mode == "missing-provider")
                {
                    assert(failure->domain == "scene.create" && failure->cause.has_value());
                    const auto *original = std::any_cast<lux::scene::SceneBuildFailure>(&failure->cause);
                    assert(original && original->code == lux::scene::ESceneBuildError::SCENE_SYSTEM_BUILD_FAILURE);
                    std::printf("expected provider rejection: %s:%llu objects=0 views=%zu\n", failure->domain.c_str(),
                                failure->reason, evidence_.renderer->statistics().views);
                    assert(editor_->documents().empty());
                    evidence_.checks += 2;
                    exit_.request(*editor_);
                    return;
                }
                std::fprintf(stderr, "FAIL open %s:%llu %s\n", failure->domain.c_str(), failure->reason,
                             failure->message.c_str());
                evidence_.failed = true;
                exit_.request(*editor_);
                return;
            }
            if (const auto *handle = std::get_if<DocumentHandle>(&*status))
            {
                handle_ = *handle;
                assert(editor_->acknowledgeOpen(second_));
                const auto duplicate = editor_->requestOpen(request_);
                assert(duplicate && std::get<DocumentHandle>(*editor_->openStatus(*duplicate)) == handle_);
                assert(editor_->acknowledgeOpen(*duplicate));
                assert(editor_->documents().size() == 1);
                ++stage_;
                evidence_.checks += 4;
            }
        }
        if (stage_ == 2)
        {
            const auto doc = editor_->document(handle_);
            assert(doc);
            auto &scene = dynamic_cast<lux::editor::scene::SceneEditor &>(doc->get());
            assert(scene.objects().size() == evidence_.expected_objects);
            assert(scene.coordinatePageSize() == (evidence_.mode == "cpu" ? 0 : 2048));
            const auto snapshot = scene.resources();
            if (!snapshot || snapshot->rows.size() != 3)
            {
                return;
            }
            std::size_t failures{}, ready{};
            for (const auto &row : snapshot->rows)
            {
                if (row.state == lux::editor::scene::ESceneResourceState::FAILED)
                {
                    if (evidence_.mode == "bad-material")
                    {
                        assert(std::holds_alternative<lux::process::asset_loading::AssetLoadFailure>(row.failure));
                        const auto &failure = std::get<lux::process::asset_loading::AssetLoadFailure>(row.failure);
                        assert(failure.code == lux::process::asset_loading::EAssetLoadError::STORAGE_FAILURE);
                        assert(failure.storage_error == lux::asset::EAssetStorageError::NOT_FOUND);
                        assert(row.failed_dependency == row.key.material);
                        std::printf("expected material failure: request=%llu load=%u dependency_present=%d\n",
                                    row.key.sequence, unsigned(failure.code), !row.failed_dependency.isNull());
                        ++failures;
                        continue;
                    }
                    std::fprintf(stderr, "FAIL resource=%llu cause=%zu\n", row.key.sequence, row.failure.index());
                    if (const auto *upload = std::get_if<lux::render::ERenderUploadSubmitError>(&row.failure))
                    {
                        std::fprintf(stderr, "upload=%u dependency=%d backend=%u\n", unsigned(*upload),
                                     !row.failed_dependency.isNull(), row.backend_status);
                    }
                    evidence_.failed = true;
                    exit_.request(*editor_);
                    return;
                }
                if (row.state != lux::editor::scene::ESceneResourceState::READY)
                {
                    return;
                }
                ++ready;
            }
            if (evidence_.mode == "bad-material")
            {
                assert(failures == 1 && ready == 2);
            }
            if (evidence_.renderer->statistics().gpu_completed < 5)
            {
                return;
            }
            const auto selected = scene.objects().front().object;
            std::size_t notices{};
            auto connection = scene.observeScoped<lux::editor::scene::SceneEditor::selectionChanged>(
                [&](const lux::editor::scene::SelectionNotice &) noexcept { ++notices; });
            assert(scene.select(selected));
            assert(notices == 1 && scene.selection().object == selected);
            assert(scene.select(selected) && notices == 1);
            const auto components = scene.components(selected);
            assert(!components.empty());
            const auto *value = static_cast<const lux::simulation::ecs::Transform3D *>(
                scene.component(selected, lux::cxx::typeToken<lux::simulation::ecs::Transform3D>()));
            assert(value && value->translation.y() == 1);
            const auto before = *scene.historyView();
            assert(before.undo == editing::EHistoryActionAvailability::EMPTY);
            assert(!scene.undo() && !scene.redo());
            assert(scene.historyView()->history.revision == before.history.revision);
            evidence_.checks += 9;
            if (evidence_.mode == "background")
            {
                assert(background_active_.load(std::memory_order_acquire));
                assert(!background_done_.load(std::memory_order_acquire));
                ++evidence_.checks;
                std::puts("document/resources/GPU ready with unrelated Process task still active");
            }
            if (evidence_.mode == "render-thread")
            {
                thread_checks_.begin(*evidence_.renderer);
                stage_ = 94;
                return;
            }
            if (evidence_.mode == "render-association")
            {
                association_checks_.begin(scene, *evidence_.renderer);
                stage_ = 91;
                return;
            }
            if (evidence_.mode == "fixed-run-failure")
            {
                run_failure_checks_.begin(scene);
                stage_ = 96;
                return;
            }
            if (evidence_.mode == "fixed-run")
            {
                run_checks_.begin(scene, *evidence_.renderer);
                stage_ = 90;
                return;
            }
            if (evidence_.mode == "resize")
            {
                auto opened =
                    evidence_.renderer->openView(*scene.renderScene(), {{256, 128}, true, scene.coordinatePageSize()});
                assert(opened);
                extra_view_ = std::move(*opened);
                rendering::CameraFrame frame;
                frame.view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -8, 1};
                frame.projection = {0.8660254, 0, 0, 0, 0, -1.7320508, 0, 0, 0, 0, -1.0005, -1, 0, 0, -0.05, 0};
                frame.desired = {scene.historyId().value, 0, 1, 1};
                assert(extra_view_->setCamera(frame));
                stage_ = 20;
                return;
            }
            if (evidence_.mode == "close-signal")
            {
                auto closing = scene.observeScoped<lux::editor::scene::SceneEditor::selectionChanged>(
                    [&](const auto &) noexcept { scene.requestClose(); });
                assert(scene.select({}));
                assert(editor_->document(handle_));
                assert(scene.closeStatus().state == ECloseState::CLOSING);
                evidence_.checks += 2;
                stage_ = 10;
                return;
            }
            if (evidence_.mode == "save-import-model")
            {
                placement_checks_.begin(scene, *runtime_);
                stage_ = 44;
                return;
            }
            for (const auto &view : scene.views())
            {
                auto *pane = dynamic_cast<gui::GuiView *>(view.get());
                assert(pane);
                pane->pane().setVisible(evidence_.mode == "save-hierarchy");
            }
            hidden_revision_ = snapshot->revision;
            if (evidence_.mode.starts_with("save"))
            {
                save_checks_.begin(scene, evidence_.mode);
                stage_ = 40;
                return;
            }
            checkSceneEditing(scene);
            before_revision_ = scene.historyView()->history.revision;
            stage_ = 5;
            return;
        }
        if (stage_ == 96)
        {
            auto& scene = dynamic_cast<lux::editor::scene::SceneEditor&>(editor_->document(handle_)->get());
            if (run_failure_checks_.poll(scene))
            {
                stage_ = 97;
                exit_.request(*editor_);
            }
            return;
        }
        if (stage_ == 94)
        {
            if (thread_checks_.poll(*runtime_, *evidence_.renderer))
            {
                stage_ = 95;
                exit_.request(*editor_);
            }
            return;
        }
        if (stage_ == 91)
        {
            return;
        }
        if (stage_ == 90)
        {
            auto &scene = dynamic_cast<lux::editor::scene::SceneEditor &>(editor_->document(handle_)->get());
            if (run_checks_.poll(scene, *evidence_.renderer))
            {
                evidence_.checks += 12;
                stage_ = 93;
                exit_.request(*editor_);
            }
            return;
        }
        if (stage_ == 44)
        {
            auto &scene = dynamic_cast<lux::editor::scene::SceneEditor &>(editor_->document(handle_)->get());
            if (placement_checks_.poll(scene, budget))
            {
                save_checks_.begin(scene, evidence_.mode, placement_checks_.object);
                stage_ = 40;
            }
            return;
        }
        if (stage_ == 40)
        {
            auto current = editor_->document(handle_);
            assert(current);
            auto &scene = dynamic_cast<lux::editor::scene::SceneEditor &>(current->get());
            if (save_checks_.finished(scene))
            {
                scene.requestClose();
                stage_ = 41;
            }
            return;
        }
        if (stage_ == 41)
        {
            if (editor_->document(handle_))
            {
                return;
            }
            auto next = editor_->requestOpen(request_);
            assert(next);
            second_ = *next;
            stage_ = 42;
            return;
        }
        if (stage_ == 42)
        {
            auto state = editor_->openStatus(second_);
            assert(state);
            if (std::holds_alternative<OpenPending>(*state))
            {
                return;
            }
            if (const auto *failure = std::get_if<EditorFailure>(&*state))
            {
                std::fprintf(stderr, "reopen error: %s %llu %s\n", failure->domain.c_str(), failure->reason,
                             failure->message.c_str());
            }
            assert(std::holds_alternative<DocumentHandle>(*state));
            auto current = editor_->document(std::get<DocumentHandle>(*state));
            assert(current);
            save_checks_.reopened(dynamic_cast<lux::editor::scene::SceneEditor &>(current->get()));
            evidence_.checks += 6;
            if (evidence_.mode == "save-import-model")
            {
                placement_checks_.closeWithPending(dynamic_cast<lux::editor::scene::SceneEditor &>(current->get()));
            }
            stage_ = 43;
            exit_.request(*editor_);
            return;
        }
        if (stage_ == 10 && !editor_->document(handle_))
        {
            assert(editor_->documents().empty());
            const auto reopened = editor_->requestOpen(request_);
            assert(reopened);
            second_ = *reopened;
            stage_ = 11;
        }
        if (stage_ == 20)
        {
            auto image = extra_view_->acquireImage();
            if (!image)
            {
                assert(image.error().code == rendering::ERendererError::NOT_READY);
                return;
            }
            held_image_ = std::move(*image);
            // Fill the owner's bounded queue without polling it. The server cannot drain this queue itself.
            std::size_t submitted{};
            while (!pending_packet_.valid())
            {
                assert(evidence_.window->beginFrame({{1600, 900}, 1.0F / 60.0F, {1, 1}}));
                auto snapshot = evidence_.window->finishFrame();
                assert(snapshot);
                const std::array images{held_image_};
                auto packet = evidence_.renderer->sealFrame(*snapshot, images);
                assert(packet && !snapshot->valid());
                const auto accepted = evidence_.renderer->trySubmitFrame(*packet);
                assert(accepted);
                if (*accepted == rendering::EFrameSubmit::BACKPRESSURED)
                {
                    assert(packet->valid());
                    pending_packet_ = std::move(*packet);
                }
                else
                {
                    assert(!packet->valid());
                    ++submitted;
                }
                assert(submitted <= 3);
            }
            assert(submitted > 0);
            packet_blocked_at_ = std::chrono::steady_clock::now();
            std::printf("backpressure: submitted=%zu retained_packet=1 old_image_lease=1\n", submitted);
            stage_ = 21;
            return;
        }
        if (stage_ == 21)
        {
            const auto submitted = evidence_.renderer->trySubmitFrame(pending_packet_);
            assert(submitted);
            if (*submitted == rendering::EFrameSubmit::BACKPRESSURED)
            {
                return;
            }
            assert(!pending_packet_.valid());
            resize_requested_at_ = std::chrono::steady_clock::now();
            for (std::uint32_t index{}; index < 8; ++index)
            {
                assert(extra_view_->requestExtent({256 + index * 16, 192}));
            }
            assert(extra_view_->requestExtent({320, 192}));
            stage_ = 22;
            return;
        }
        if (stage_ == 22)
        {
            const auto state = extra_view_->status();
            const auto completed = evidence_.renderer->imageEvidence(held_image_);
            assert(completed);
            if (completed->evidence != rendering::EImageEvidence::GPU_COMPLETE)
            {
                return;
            }
            assert(held_image_.extent == rendering::PixelExtent(256, 128) && held_image_.lease.valid());
            assert(state.state == rendering::EViewState::RESIZING);
            std::printf("resize held: old=256x128 requested=320x192 actual_frame=%llu GPU_COMPLETE state=RESIZING\n",
                        completed->frame_serial);
            held_image_ = {};
            image_released_at_ = std::chrono::steady_clock::now();
            stage_ = 25;
            return;
        }
        if (stage_ == 25)
        {
            const auto state = extra_view_->status();
            if (state.state != rendering::EViewState::READY || state.ready_extent != rendering::PixelExtent{320, 192})
            {
                return;
            }
            auto next = extra_view_->acquireImage();
            assert(next);
            resize_ready_at_ = std::chrono::steady_clock::now();
            held_image_ = std::move(*next);
            assert(extra_view_->beginClose());
            const auto close = extra_view_->advanceClose();
            assert(close && *close == rendering::ERenderClose::PENDING);
            std::printf("resize complete: latest=320x192; close with new lease=PENDING\n");
            held_image_ = {};
            stage_ = 23;
            return;
        }
        if (stage_ == 23)
        {
            const auto close = extra_view_->advanceClose();
            assert(close);
            if (*close != rendering::ERenderClose::COMPLETE)
            {
                return;
            }
            extra_view_.reset();
            const auto closed_at = std::chrono::steady_clock::now();
            const auto us = [](auto elapsed)
            { return std::chrono::duration<double, std::micro>(elapsed).count(); };
            std::printf("MEASURE resize packet_retry_us=%.3f held_old_us=%.3f release_to_ready_us=%.3f "
                        "ready_to_closed_us=%.3f views=2 resize_requests=9 old=256x128 new=320x192 "
                        "cpu_lease_and_GPU_COMPLETE_checked=1\n",
                        us(resize_requested_at_ - packet_blocked_at_),
                        us(image_released_at_ - resize_requested_at_), us(resize_ready_at_ - image_released_at_),
                        us(closed_at - resize_ready_at_));
            evidence_.checks += 8;
            stage_ = 24;
            exit_.request(*editor_);
        }
        if (stage_ == 11)
        {
            const auto state = editor_->openStatus(second_);
            assert(state);
            if (const auto *current = std::get_if<DocumentHandle>(&*state))
            {
                assert(*current != handle_ && !editor_->document(handle_));
                assert(editor_->acknowledgeOpen(second_));
                evidence_.checks += 3;
                stage_ = 12;
                exit_.request(*editor_);
            }
            return;
        }
        if (stage_ == 5 && evidence_.frames >= 30)
        {
            auto &scene = dynamic_cast<lux::editor::scene::SceneEditor &>(editor_->document(handle_)->get());
            assert(scene.resources()->revision == hidden_revision_);
            const auto selection =
                evidence_.mode == "cost" ? scene.objects().front().object : scene.objects().back().object;
            assert(scene.select(selection));
            for (const auto &view : scene.views())
            {
                dynamic_cast<gui::GuiView *>(view.get())->pane().setVisible(true);
            }
            assert(scene.selection().object == selection && scene.historyView()->history.revision == before_revision_);
            evidence_.checks += 3;
            stage_ = 3;
        }
        if (evidence_.mode == "pane-lifecycle" && (stage_ == 3 || stage_ >= 80))
        {
            auto &scene = dynamic_cast<lux::editor::scene::SceneEditor &>(editor_->document(handle_)->get());
            if (stage_ == 3)
            {
                assert(scene.views().size() == 5);
                const auto prefix = "scene-" + std::to_string(scene.historyId().value);
                std::size_t index{};
                for (const auto *suffix : {"-inspector", "-outliner"})
                {
                    const auto found = std::ranges::find_if(scene.views(), [&](const auto &view)
                                                            { return view->id() == prefix + suffix; });
                    assert(found != scene.views().end());
                    const auto target = dynamic_cast<gui::GuiView &>(**found).pane().weakRef();
                    retired_panes_[index++] = target;
                    assert(lux::object::detail::post(scene.dispatcherRef(),
                                                     lux::object::detail::makeMessage(
                                                         [this, target]() noexcept
                                                         {
                                                             assert(target.expired() && !target.getOnCurrent());
                                                             ++retired_messages_;
                                                         })) == lux::object::detail::EPostStatus::POSTED);
                    (*found)->requestClose();
                }
                const auto pending =
                    gui::sceneDocumentProvider().attach(scene, *evidence_.window, *evidence_.renderer, *runtime_);
                assert(!pending && pending.error().code == EEditorError::BUSY);
                assert(scene.views().size() == 5);
                before_revision_ = scene.historyView()->history.revision;

                // The frontend frame has finished. Advance the real document owner
                // here, before the next dispatcher batch, never from a notification
                // callback.
                scene.poll(budget);
                assert(scene.views().size() == 3 && retired_messages_ == 0);
                assert(retired_panes_[0].expired() && retired_panes_[1].expired());
                std::puts("C19 retired Inspector and Outliner with two queued "
                          "weak-target messages");
                stage_ = 80;
            }
            if (stage_ == 80)
            {
                assert(scene.closeStatus().state == ECloseState::OPEN);
                assert(scene.historyView()->history.revision == before_revision_);
                checkSceneEditing(scene);
                std::puts("C19 absent selection begin");
                assert(scene.select(scene.objects().front().object));
                std::puts("C19 absent selection end");
                const auto before = scene.historyView()->history;
                const auto rebuilt =
                    gui::sceneDocumentProvider().attach(scene, *evidence_.window, *evidence_.renderer, *runtime_);
                if (!rebuilt)
                {
                    std::fprintf(stderr,
                                 "Pane rebuild rejected: domain=%s reason=%llu views=%zu "
                                 "revision=%llu\n",
                                 rebuilt.error().domain.c_str(), rebuilt.error().reason, scene.views().size(),
                                 before.revision.value);
                }
                assert(rebuilt && scene.views().size() == 5);
                assert(scene.historyView()->history.current == before.current);
                assert(scene.historyView()->history.revision == before.revision);
                assert(gui::sceneDocumentProvider().attach(scene, *evidence_.window, *evidence_.renderer, *runtime_));
                assert(scene.views().size() == 5);
                const auto owner = scene.weakRef();
                const auto selected = scene.objects().back().object;
                assert(lux::object::detail::post(scene.dispatcherRef(),
                                                 lux::object::detail::makeMessage(
                                                     [this, owner, selected]() noexcept
                                                     {
                                                         auto *document = owner.getAsOnCurrent<scene::SceneEditor>();
                                                         assert(document && retired_messages_ == 2);
                                                         std::puts("C19 rebuilt selection begin");
                                                         assert(document->select(selected));
                                                         std::puts("C19 rebuilt selection end");
                                                         ++rebuilt_messages_;
                                                     })) == lux::object::detail::EPostStatus::POSTED);
                material_frame_ = evidence_.frames;
                stage_ = 81;
                return;
            }
            if (stage_ == 81 && evidence_.frames >= material_frame_ + 10)
            {
                assert(retired_messages_ == 2 && rebuilt_messages_ == 1);
                assert(retired_panes_[0].expired() && retired_panes_[1].expired());
                assert(scene.selection().object == scene.objects().back().object);
                checkSceneEditing(scene);
                std::puts("PASS C19 owner/queue integration: actual Inspector and "
                          "Outliner destroyed and rebuilt; "
                          "two old weak targets expired before queued delivery; queued "
                          "document selection reaches rebuilt "
                          "Panes; document/history and other three views retained; "
                          "edits/Undo/Redo work; attach is idempotent");
                evidence_.checks += 14;
                stage_ = 82;
                exit_.request(*editor_);
            }
            return;
        }
        if (evidence_.mode == "flow-gui")
        {
            if (stage_ == 3 && evidence_.frames >= 50)
            {
                const auto &assets = project_->manifest().assets;
                const auto found = std::ranges::find(assets, EProjectAssetKind::FLOW_GRAPH, &ProjectAssetEntry::kind);
                assert(found != assets.end() && evidence_.window);
                evidence_.window->openAsset(found->id);
                stage_ = 60;
            }
            if (stage_ == 60)
            {
                for (const auto &summary : editor_->documents())
                {
                    if (summary.key.type != lux::editor::flowforge::kFlowForgeDocumentType)
                    {
                        continue;
                    }
                    auto &doc = dynamic_cast<lux::editor::flowforge::FlowForgeEditor &>(
                        editor_->document(summary.handle)->get());
                    if (doc.views().empty())
                    {
                        return;
                    }
                    material_ = summary.handle;
                    assert(doc.rename("FlowForge pane binding") && doc.undo());
                    assert(doc.redo());
                    checkLocalHistory(
                        dynamic_cast<lux::editor::scene::SceneEditor &>(editor_->document(handle_)->get()), doc);
                    assert(doc.undo());
                    std::unique_ptr<lux::flowforge::Node> branch = std::make_unique<lux::flowforge::BranchNode>(0);
                    assert(doc.insertNode(branch, {280, 80, true}));
                    material_frame_ = evidence_.frames;
                    stage_ = 61;
                    std::puts("FlowForge GUI: Project/Window asset signal opened real FlowForgeEditor and attached "
                              "node canvas");
                }
            }
            if (stage_ == 61 && evidence_.frames >= material_frame_ + 30)
            {
                editor_->document(handle_)->get().requestClose();
                material_frame_ = evidence_.frames;
                stage_ = 62;
            }
            if (stage_ == 62 && evidence_.frames >= material_frame_ + 30 && !editor_->document(handle_))
            {
                auto &doc =
                    dynamic_cast<lux::editor::flowforge::FlowForgeEditor &>(editor_->document(material_)->get());
                assert(doc.views().size() == 1 && doc.closeStatus().state == ECloseState::OPEN);
                assert(evidence_.renderer->statistics().views == 0);
                std::puts("FlowForge GUI: continued drawing after Scene closed; local FlowForge owner and history "
                          "remain usable");
                assert(doc.rename("Still editable") && doc.undo());
                exit_.request(*editor_);
                stage_ = 63;
            }
            return;
        }
        if (evidence_.mode == "material-gui" || evidence_.mode == "material-publish")
        {
            if (stage_ == 3 && evidence_.frames >= 50)
            {
                const auto &assets = project_->manifest().assets;
                const auto found =
                    std::ranges::find(assets, EProjectAssetKind::MATERIAL_GRAPH, &ProjectAssetEntry::kind);
                assert(found != assets.end() && evidence_.window);
                evidence_.window->openAsset(found->id);
                stage_ = 60;
            }
            if (stage_ == 60)
            {
                for (const auto &summary : editor_->documents())
                {
                    if (summary.key.type != lux::editor::material::kMaterialDocumentType)
                    {
                        continue;
                    }
                    auto &doc =
                        dynamic_cast<lux::editor::material::MaterialEditor &>(editor_->document(summary.handle)->get());
                    if (doc.views().empty())
                    {
                        return;
                    }
                    material_ = summary.handle;
                    assert(doc.rename("Material pane binding") && doc.undo());
                    assert(doc.redo());
                    checkLocalHistory(
                        dynamic_cast<lux::editor::scene::SceneEditor &>(editor_->document(handle_)->get()), doc);
                    assert(doc.undo());
                    material_frame_ = evidence_.frames;
                    stage_ = 61;
                    if (evidence_.mode == "material-publish")
                    {
                        material_compile_ = *doc.requestCompile();
                        stage_ = 70;
                    }
                    else
                    {
                        const auto &assets = project_->manifest().assets;
                        const auto second =
                            std::ranges::find(assets, "Unfinished.luxmaterial", &ProjectAssetEntry::source_path);
                        assert(second != assets.end());
                        evidence_.window->openAsset(second->id);
                        stage_ = 65;
                    }
                    std::puts("Material GUI: Project/Window asset signal opened real MaterialEditor and attached node "
                              "canvas");
                }
            }
            if (stage_ == 65)
            {
                for (const auto &summary : editor_->documents())
                {
                    if (summary.key.type == material::kMaterialDocumentType && summary.handle != material_)
                    {
                        auto &second =
                            dynamic_cast<material::MaterialEditor &>(editor_->document(summary.handle)->get());
                        if (second.views().empty())
                        {
                            return;
                        }
                        checkThreeHistories(
                            dynamic_cast<lux::editor::scene::SceneEditor &>(editor_->document(handle_)->get()),
                            dynamic_cast<material::MaterialEditor &>(editor_->document(material_)->get()), second);
                        material_frame_ = evidence_.frames;
                        stage_ = 61;
                        break;
                    }
                }
            }
            if (stage_ >= 70 && stage_ <= 75)
            {
                auto &doc = dynamic_cast<lux::editor::material::MaterialEditor &>(editor_->document(material_)->get());
                auto &scene = dynamic_cast<lux::editor::scene::SceneEditor &>(editor_->document(handle_)->get());
                if (stage_ == 70 || stage_ == 73)
                {
                    const auto status = doc.compileStatus(material_compile_);
                    assert(status);
                    if (std::holds_alternative<material::MaterialCompilePending>(*status))
                    {
                        return;
                    }
                    if (const auto *error = std::get_if<material::MaterialCompileFailed>(&*status))
                    {
                        std::printf("MATERIAL COMPILE ERROR %s:%llu %s\n", error->failure.domain.c_str(),
                                    error->failure.reason, error->failure.message.c_str());
                    }
                    assert(std::holds_alternative<material::MaterialCompileSucceeded>(*status));
                    material_save_ = *doc.requestPublish(material_compile_, "gpu-test");
                    assert(doc.saveRequests().size() == 1 && doc.saveRequests().front() == material_save_);
                    assert(doc.acknowledgeCompile(material_compile_));
                    ++stage_;
                }
                if (stage_ == 71 || stage_ == 74)
                {
                    const auto status = doc.saveStatus(material_save_);
                    assert(status);
                    if (std::holds_alternative<SavePending>(*status))
                    {
                        return;
                    }
                    if (const auto *error = std::get_if<SaveRetryable>(&*status))
                    {
                        std::printf("MATERIAL PUBLISH ERROR %s:%llu %s\n", error->failure.domain.c_str(),
                                    error->failure.reason, error->failure.message.c_str());
                    }
                    assert(std::holds_alternative<SaveSucceeded>(*status) && doc.acknowledgeSave(material_save_));
                    if (stage_ == 71)
                    {
                        const auto row = std::ranges::find_if(
                            scene.objects(),
                            [&](const auto &object)
                            {
                                return scene.component(object.object,
                                                       lux::cxx::typeToken<lux::simulation::ecs::Mesh3D>());
                            });
                        assert(row != scene.objects().end());
                        using Mesh = lux::simulation::ecs::Mesh3D;
                        auto value =
                            static_cast<const Mesh *>(scene.component(row->object, lux::cxx::typeToken<Mesh>()))->value;
                        value.material = doc.summary().key.source;
                        auto edit = scene.setField<Mesh>(
                            *scene.writeTarget(row->object), "Mesh3D.value", "Material",
                            [](auto &mesh) noexcept { return &mesh.value; }, value);
                        assert(edit);
                    }
                    ++stage_;
                }
                if (stage_ == 72 || stage_ == 75)
                {
                    const auto resources = scene.resources();
                    assert(resources);
                    const auto material_id = doc.summary().key.source;
                    const auto ready = std::ranges::find_if(resources->rows,
                                                            [&](const auto &row)
                                                            {
                                                                return row.key.material == material_id &&
                                                                       row.state == scene::ESceneResourceState::READY &&
                                                                       !row.refresh_pending &&
                                                                       row.key.sequence > material_resource_;
                                                            });
                    for (const auto &row : resources->rows)
                    {
                        if (row.state == scene::ESceneResourceState::FAILED)
                        {
                            std::printf("MATERIAL GPU ERROR request=%llu cause=%zu backend=%u\n", row.key.sequence,
                                        row.failure.index(), row.backend_status);
                            assert(false);
                        }
                    }
                    if (evidence_.frames % 120 == 0)
                    {
                        const auto stats = evidence_.renderer->statistics();
                        std::printf("publication progress stage=%u frame=%zu gpu=%llu resources=%zu ready=%d\n", stage_,
                                    evidence_.frames, stats.gpu_completed, resources->rows.size(),
                                    ready != resources->rows.end());
                        for (const auto &row : resources->rows)
                        {
                            std::printf("  request=%llu match=%d state=%u refresh=%d render=%u backend=%u\n",
                                        row.key.sequence, row.key.material == material_id, unsigned(row.state),
                                        row.refresh_pending, unsigned(!row.render_failure.ok()), row.backend_status);
                        }
                    }
                    if (ready == resources->rows.end())
                    {
                        return;
                    }
                    const bool retired =
                        std::ranges::none_of(resources->rows, [](const auto &row)
                                             { return row.state == scene::ESceneResourceState::SUPERSEDED; });
                    if (!retired || evidence_.renderer->statistics().gpu_completed < material_watermark_ + 5)
                    {
                        return;
                    }
                    const auto next = ready->key.sequence;
                    if (stage_ == 72)
                    {
                        material_resource_ = next;
                        material_watermark_ = evidence_.renderer->statistics().gpu_completed;
                        material_cooked_ = project_->asset(material_id)->cooked_path;
                        for (const auto &node : doc.source().graph.topology().nodes())
                        {
                            if (doc.source().graph.node(node.id)->as<lux::material::ConstantNode>())
                            {
                                assert(doc.setConstant(node.id, {0.1F, 0.8F, 0.2F, 0}));
                                break;
                            }
                        }
                        material_compile_ = *doc.requestCompile();
                        stage_ = 73;
                    }
                    else
                    {
                        assert(project_->asset(material_id)->cooked_path != material_cooked_);
                        std::printf("GPU material publication: same AssetId request %llu -> %llu; immutable pak "
                                    "changed; old request retired; gpu completion %llu -> %llu\n",
                                    material_resource_, next, material_watermark_,
                                    evidence_.renderer->statistics().gpu_completed);
                        editor_->document(handle_)->get().requestClose();
                        material_frame_ = evidence_.frames;
                        stage_ = 62;
                    }
                }
                return;
            }
            if (stage_ == 61 && evidence_.frames >= material_frame_ + 30)
            {
                editor_->document(handle_)->get().requestClose();
                material_frame_ = evidence_.frames;
                stage_ = 62;
            }
            if (stage_ == 62 && evidence_.frames >= material_frame_ + 30 && !editor_->document(handle_))
            {
                auto &doc = dynamic_cast<lux::editor::material::MaterialEditor &>(editor_->document(material_)->get());
                assert(doc.views().size() == 1 && doc.closeStatus().state == ECloseState::OPEN);
                assert(evidence_.renderer->statistics().views == 0);
                std::puts("Material GUI: continued drawing after Scene closed; local material owner and history remain "
                          "usable");
                assert(doc.rename("Still editable") && doc.undo());
                exit_.request(*editor_);
                stage_ = 63;
            }
            return;
        }
        if (stage_ == 3 && (evidence_.mode == "cost" ? evidence_.cost_draws == 120 : evidence_.frames >= 50))
        {
            const auto stats = evidence_.renderer->statistics();
            std::printf("real scene: checks=%zu frames=%zu gpu_completed=%llu views=%zu leases=%zu\n", evidence_.checks,
                        evidence_.frames, stats.gpu_completed, stats.views, stats.runtime_leases);
            assert(stats.views == (evidence_.mode == "cpu" ? 0 : 1) && stats.gpu_completed > 0);
            exit_.request(*editor_);
            ++stage_;
        }
    }
    void draw(Editor &editor, PollBudget &budget) override
    {
        SampleTime sample{evidence_.callbacks};
        const bool measure = evidence_.mode == "cost" && stage_ == 3 && evidence_.cost_draws < 120;
        const auto captured_before = measure ? evidence_.window->capturedFrames() : 0;
        const auto begin = std::chrono::steady_clock::now();
        inner_->draw(editor, budget);
        if (measure)
        {
            const auto elapsed = std::chrono::steady_clock::now() - begin;
            const auto captures = evidence_.window->capturedFrames() - captured_before;
            assert(captures <= 1);
            if (captures == 1)
            {
                if (evidence_.cost_draws >= 20)
                {
                    evidence_.cost_draw_time += elapsed;
                }
                ++evidence_.cost_draws;
            }
            else if (evidence_.cost_draws >= 20)
            {
                ++evidence_.cost_retries;
                evidence_.cost_retry_time += elapsed;
            }
        }
        ++evidence_.frames;
    }
    void wait() override
    {
        SampleTime sample{evidence_.waits};
        inner_->wait();
    }
    void stopPresenting() noexcept override
    {
        inner_->stopPresenting();
    }
    void requestClose() noexcept override
    {
        closing_ = true;
        assert(editor_->documents().empty());
        if (evidence_.renderer)
        {
            const auto stats = evidence_.renderer->statistics();
            assert(stats.views == 0 && stats.runtime_leases == 0);
            if (evidence_.mode == "fixed-run" || evidence_.mode == "fixed-run-failure" ||
                evidence_.mode == "render-association" || evidence_.mode == "render-thread")
            {
                assert(stats.validation_errors == 0);
                std::printf("D3 GPU facts: submitted_frames=%llu completed=%llu validation_errors=%llu\n",
                            static_cast<unsigned long long>(stats.frames),
                            static_cast<unsigned long long>(stats.gpu_completed),
                            static_cast<unsigned long long>(stats.validation_errors));
            }
            std::printf("scene owners drained: views=%zu leases=%zu\n", stats.views, stats.runtime_leases);
        }
        inner_->requestClose();
        if (evidence_.mode == "background")
        {
            background_stop_.store(true, std::memory_order_release);
            background_stop_.notify_one();
        }
        const auto joined = stdexec::sync_wait(background_.close());
        assert(joined);
        assert(evidence_.mode != "background" || background_done_.load(std::memory_order_acquire));
    }
    CloseStatus closeStatus() const override
    {
        const auto status = inner_->closeStatus();
        if (status.state == ECloseState::CLOSED)
        {
            evidence_.closed = true;
        }
        return status;
    }

  private:
    DocumentHandle material_;
    std::size_t material_frame_{};
    SceneSaveChecks save_checks_;
    ModelPlacementChecks placement_checks_;
    SceneRunChecks run_checks_;
    RunFailureChecks run_failure_checks_;
    RenderAssociationChecks association_checks_;
    RenderThreadChecks thread_checks_;
    lux::process::ExecutionRuntime *runtime_{};
    Evidence &evidence_;
    std::unique_ptr<EditorFrontend> inner_;
    Editor *editor_{};
    Project *project_{};
    OpenDocumentRequest request_;
    OpenRequestId first_, second_;
    DocumentHandle handle_;
    std::chrono::steady_clock::time_point started_;
    lux::editor::material::MaterialCompileId material_compile_;
    SaveRequestId material_save_;
    std::uint64_t material_resource_{}, material_watermark_{};
    std::string material_cooked_;
    unsigned stage_{};
    std::uint64_t hidden_revision_{};
    editing::Revision before_revision_{};
    std::array<lux::object::ObjectWeakRef, 2> retired_panes_;
    unsigned retired_messages_{}, rebuilt_messages_{};
    std::chrono::steady_clock::time_point packet_blocked_at_, resize_requested_at_, image_released_at_, resize_ready_at_;
    std::unique_ptr<rendering::RenderView> extra_view_;
    rendering::ViewImage held_image_;
    rendering::EditorFramePacket pending_packet_;
    lux::process::TaskScope background_;
    std::atomic_bool background_active_{}, background_stop_{}, background_done_{};
    bool closing_{};
};

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    assert(argc >= 2 && argc <= 4);
    lux::meta::ReflectionRegistry::initRegistry();
    Evidence evidence;
    if (argc >= 3)
    {
        evidence.mode = argv[2];
    }
    if (argc == 4)
    {
        assert(evidence.mode == "cost");
        evidence.expected_objects = std::stoul(argv[3]);
    }
    gui::GuiConfig gui;
    gui.window.visible = false;
    if (evidence.mode == "invalid-window")
    {
        gui.window.width = 0;
    }
    if (evidence.mode == "invalid-renderer")
    {
        gui.renderer.frame_capacity = 0;
    }
    gui.renderer.validation = evidence.mode != "cost";
    gui.renderer.validation_message_sink = [&evidence](std::uint32_t severity, std::string_view message)
    {
        std::fprintf(stderr, "Vulkan %u: %.*s\n", severity, int(message.size()), message.data());
        if (severity == 2 || message.find("Validation Error") != std::string_view::npos)
        {
            evidence.failed = true;
        }
    };
    auto provider = gui::sceneDocumentProvider();
    const auto registration = provider.register_type;
    provider.register_type = [&evidence, registration](Editor &editor, lux::process::ExecutionRuntime &runtime,
                                                       rendering::EditorRenderer &renderer)
    {
        evidence.renderer = &renderer;
        return registration(editor, runtime, renderer);
    };
    const auto attach = provider.attach;
    provider.attach = [&evidence, attach](DocumentEditor &document, gui::EditorWindow &window,
                                          rendering::EditorRenderer &renderer,
                                          lux::process::ExecutionRuntime &runtime) -> EditorResult<void>
    {
        evidence.window = &window;
        if (evidence.mode == "attach-rollback" && !evidence.rollback)
        {
            struct CollisionPane final : lux::object::Object<CollisionPane, lux::ui::Pane>
            {
                CollisionPane(lux::object::ObjectDispatcherRef dispatcher, std::string name)
                    : Object(dispatcher, lux::ui::PaneId{std::move(name)}, lux::ui::PaneTypeId{"probe"}, "collision")
                {
                }
                void draw(lux::ui::Frame &, lux::ui::PaneDrawContext &) override {}
            };
            CollisionPane collision(window.dispatcherRef(),
                                    "scene-" + std::to_string(document.historyId().value) + "-inspector");
            auto registered = window.uiSession().registerPane(collision);
            assert(registered);
            auto result = attach(document, window, renderer, runtime);
            assert(!result && result.error().domain == "ui.register" &&
                   result.error().reason ==
                       static_cast<std::uint64_t>(lux::ui::EUiRegistrationError::DUPLICATE_PANE_ID));
            assert(document.views().empty() && renderer.statistics().views == 0);
            std::printf("expected attach rejection: %s:%llu adopted_views=0 GPU_views=0\n",
                        result.error().domain.c_str(), result.error().reason);
            registered->reset();
            evidence.rollback = true;
            evidence.checks += 3;
        }
        return attach(document, window, renderer, runtime);
    };
    gui.providers.push_back(std::move(provider));
    if (evidence.mode == "material-gui" || evidence.mode == "material-publish")
    {
        gui.providers.push_back(gui::materialDocumentProvider());
    }
    if (evidence.mode == "flow-gui")
    {
        gui.providers.push_back(gui::flowForgeDocumentProvider(flowMetadata(std::make_shared<MetadataOwner>())));
    }
    EditorConfig config;
    config.project_file = argv[1];
    config.execution = {2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}};
    config.frontend = [&evidence, gui] { return std::make_unique<Probe>(evidence, gui); };
    Editor editor(std::move(config));
    const auto begin = std::chrono::steady_clock::now();
    const auto result = editor.exec();
    const auto expected = evidence.mode == "invalid-window" || evidence.mode == "invalid-renderer" ? 5
                          : evidence.mode == "missing-project"                                     ? 4
                                                                                                   : 0;
    assert(result == expected && evidence.closed && !evidence.failed);
    if (evidence.mode == "missing-project")
    {
        assert(!editor.outcome() && editor.outcome().error().domain == "project.read");
    }
    if (evidence.mode == "attach-rollback")
    {
        assert(evidence.rollback);
    }
    std::printf("mode=%s exit=%d closed=%d\n", evidence.mode.c_str(), result, evidence.closed);
    std::printf("PASS case=%s checks=%zu\n", evidence.mode.c_str(), evidence.checks);
    const auto milliseconds = [](auto value) { return std::chrono::duration<double, std::milli>(value).count(); };
    std::printf("timing: exec_ms=%.3f frontend_callbacks_ms=%.3f frontend_wait_ms=%.3f\n",
                milliseconds(std::chrono::steady_clock::now() - begin), milliseconds(evidence.callbacks),
                milliseconds(evidence.waits));
    if (evidence.mode == "cost")
    {
        assert(evidence.cost_draws == 120);
        std::printf("MEASURE desktop objects=%zu render_objects=4 warmup=20 captured_ui_frames=100 active_draw_ms=%.3f "
                    "retry_calls=%zu active_retry_ms=%.3f width=1600 height=900 scene_views=1\n",
                    evidence.expected_objects, milliseconds(evidence.cost_draw_time), evidence.cost_retries,
                    milliseconds(evidence.cost_retry_time));
    }
}

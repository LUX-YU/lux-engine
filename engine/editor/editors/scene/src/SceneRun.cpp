#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>
#include <cassert>
#include <lux/engine/editor/detail/DocumentTask.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/editor/scene/detail/SceneObjects.hpp>
#include <lux/engine/editor/scene/detail/SceneRun.hpp>
#include <lux/engine/scene/MeshQuerySystem.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/SceneDriver.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <lux/engine/scene/WorldMaterializer.hpp>
#include <lux/engine/simulation/ecs/ComponentChangeSet.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>

namespace lux::editor::scene::detail
{
namespace
{
using Clock = std::chrono::steady_clock;

auto invalid(std::string domain, EEditorError code = EEditorError::INVALID_STATE)
{
    return lux::cxx::unexpected(EditorFailure{code, std::move(domain)});
}

struct DecodeRun final
{
    SceneCapture capture;
    std::stop_token stop;
    EditorResult<NativeScene> operator()() noexcept
    {
        auto encoded = encodeNativeScene(capture, 256U * 1024U * 1024U, stop);
        if (!encoded)
        {
            return lux::cxx::unexpected(EditorFailure{encoded.error().code == ENativeSceneError::CANCELLED
                                                          ? EEditorError::CANCELLED
                                                          : EEditorError::SOURCE_FAILURE,
                                                      "run.encode",
                                                      static_cast<std::uint64_t>(encoded.error().code),
                                                      {},
                                                      encoded.error()});
        }
        auto bytes = std::make_shared<const std::vector<std::byte>>(std::move(*encoded));
        // Keep the fixed package/volumes. WorldLoading decodes only its
        // requested partitions instead of constructing a second full set.
        auto source = decodeNativeScene(lux::cxx::SharedBytes<>::fromOwner(bytes, *bytes), stop, false);
        if (!source)
        {
            return lux::cxx::unexpected(EditorFailure{source.error().code == ENativeSceneError::CANCELLED
                                                          ? EEditorError::CANCELLED
                                                          : EEditorError::SOURCE_FAILURE,
                                                      "run.decode",
                                                      static_cast<std::uint64_t>(source.error().code),
                                                      {},
                                                      source.error()});
        }
        return std::move(*source);
    }
};
using DecodeTask = lux::editor::detail::ScheduledDocumentTask<process::CpuScheduler, DecodeRun>;

// This object is constructed, advanced and destroyed exclusively by Main.
// TaskExecutor may parallelize a synchronous step; Process only decodes input.
struct ActiveRun final
{
    std::unique_ptr<lux::scene::SceneInstance> scene;
    SceneObjects objects;
    lux::task::TaskExecutor executor;
    lux::scene::SceneDriver driver;
    std::unique_ptr<editing::EditHistory> pause_history;
    bool catalog_dirty{}, hierarchy_dirty{};
    std::vector<lux::simulation::ecs::Entity> changed_entities;
    std::vector<entt::scoped_connection> catalog_connections;
    std::vector<bool> selected_components;

    void entityChanged(lux::simulation::ecs::Registry &, lux::simulation::ecs::Entity entity)
    {
        changed_entities.push_back(entity);
    }

    void hierarchyChanged(lux::simulation::ecs::Registry &, lux::simulation::ecs::Entity)
    {
        hierarchy_dirty = true;
    }

    void refreshObjects()
    {
        namespace ecs = lux::simulation::ecs;
        auto &registry = scene->registry();
        bool changed = !changed_entities.empty() || std::exchange(hierarchy_dirty, false);
        for (const auto entity : changed_entities)
        {
            const auto ref = objects.reference(entity);
            if (!registry.valid(entity))
            {
                objects.identities.unbind(entity);
                std::erase_if(objects.rows, [&](const auto &row) { return row.object == ref; });
                std::erase_if(objects.component_versions, [&](const auto &row) { return row.object == ref; });
                if (objects.selection.object == ref)
                {
                    objects.selection.object = objects.reference(ecs::NullEntity);
                }
            }
            else if (std::ranges::find(objects.rows, ref, &SceneObjectRow::object) == objects.rows.end())
            {
                lux::partition::PartitionOrdinal partition;
                static_cast<void>(objects.loading.partitionOf(entity, partition));
                objects.rows.push_back({ref, {}, "Runtime object", partition});
            }
        }
        changed_entities.clear();
        if (changed)
        {
            for (auto &row : objects.rows)
            {
                const auto entity = objects.resolve(row.object);
                const auto *parent = registry.try_get<ecs::Parent>(entity);
                row.parent = objects.reference(parent ? parent->entity : ecs::NullEntity);
            }
            std::ranges::sort(objects.rows, std::less<SceneEntityRef>{}, &SceneObjectRow::object);
        }
        // The Inspector needs only the selected entity's component directory.
        // No per-step copy or scan of all component values is required.
        const auto selected = objects.resolve(objects.selection.object);
        std::size_t index{};
        for (const auto &schema : objects.metadata.all())
        {
            const bool present = selected != ecs::NullEntity && schema.operations.has(registry, selected);
            changed |= selected_components[index] != present;
            selected_components[index++] = present;
        }
        catalog_dirty |= changed;
    }

    ActiveRun(std::unique_ptr<lux::scene::SceneInstance> value, const NativeScene &source,
              const lux::simulation::ecs::ComponentSchemaSet &metadata, lux::task::TaskExecutor tasks)
        : scene(std::move(value)), objects(*scene, source, metadata), executor(std::move(tasks)), driver(executor)
    {
        auto &registry = scene->registry();
        using Entity = lux::simulation::ecs::Entity;
        using Parent = lux::simulation::ecs::Parent;
        catalog_connections.emplace_back(registry.on_construct<Entity>().connect<&ActiveRun::entityChanged>(*this));
        catalog_connections.emplace_back(registry.on_destroy<Entity>().connect<&ActiveRun::entityChanged>(*this));
        catalog_connections.emplace_back(registry.on_construct<Parent>().connect<&ActiveRun::hierarchyChanged>(*this));
        catalog_connections.emplace_back(registry.on_update<Parent>().connect<&ActiveRun::hierarchyChanged>(*this));
        catalog_connections.emplace_back(registry.on_destroy<Parent>().connect<&ActiveRun::hierarchyChanged>(*this));
        selected_components.resize(metadata.all().size());
    }

    ~ActiveRun()
    {
        endPause();
        driver.stop(*scene);
    }

    void endPause()
    {
        if (pause_history)
        {
            const auto closed = pause_history->close();
            assert(closed);
            pause_history.reset();
        }
    }

    EditorResult<void> beginPause()
    {
        assert(!pause_history);
        auto history = editing::EditHistory::create({kSceneHistoryLimits, {}, true});
        if (!history)
        {
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::SOURCE_FAILURE, "run.pause.history", 0, {}, history.error()});
        }
        pause_history = std::move(*history);
        return {};
    }
};
} // namespace

struct SceneRun::Data final
{
    Data(process::ExecutionRuntime &process, process::TaskScope &tasks, lux::render::RenderRuntime &render,
         SceneEditorMetadata meta, std::shared_ptr<SceneRunSlot> run_slot)
        : runtime(process), tasks(tasks), renderer(render), metadata(std::move(meta)), slot(std::move(run_slot))
    {
    }

    process::ExecutionRuntime &runtime;
    process::TaskScope &tasks;
    lux::render::RenderRuntime &renderer;
    SceneEditorMetadata metadata;
    std::shared_ptr<SceneRunSlot> slot;
    RunStatus status;
    std::uint64_t next_id{1};
    std::chrono::nanoseconds delta;
    std::stop_source stop;
    std::shared_ptr<lux::scene::RenderAssetSource> assets;
    std::unique_ptr<DecodeTask> decoding;
    std::unique_ptr<NativeScene> source;
    std::unique_ptr<ActiveRun> active;
    lux::render::RenderSceneReceipt receipt;
    double page_size{};

    EditorResult<void> install()
    {
        auto created = instantiateNativeScene(*source, metadata, tasks, renderer, assets,
                                              lux::simulation::ESimulationMode::EVOLUTION, delta);
        if (!created)
        {
            return lux::cxx::unexpected(created.error());
        }
        if (auto *render = (*created)->findSceneSystem<lux::scene::RenderSystem>())
        {
            receipt = render->resourceReceipt();
            page_size = render->coordinatePageSize();
        }
        auto executor = lux::task::TaskExecutor::create({0, 1024});
        if (!executor)
        {
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::EXECUTION_FAILURE, "run.executor", 0, {}, executor.error()});
        }
        active = std::make_unique<ActiveRun>(std::move(*created), *source, metadata.components, std::move(*executor));
        const auto played = active->driver.play(*active->scene);
        if (!played)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "run.play", 0, {}, played.error()});
        }
        status.state = ERunState::RUNNING;
        return {};
    }

    void observeRender()
    {
        const auto resource = receipt.status();
        status.render_scene = resource.scene;
        if (status.result && !resource.failure.ok())
        {
            status.result = lux::cxx::unexpected(
                EditorFailure{EEditorError::SOURCE_FAILURE, "run.render", resource.request, {}, resource.failure});
            status.failed_phase = status.render_scene.isValid() ? ERunPhase::PUBLICATION : ERunPhase::STARTUP;
            status.state = ERunState::STOPPING;
            stop.request_stop();
        }
        if (active)
        {
            if (const auto *render = active->scene->findSceneSystem<lux::scene::RenderSystem>())
            {
                const auto facts = render->transportStatistics();
                status.published_updates = facts.published;
                status.forwarded_updates = facts.forwarded;
                status.retired_updates = facts.retired_unforwarded;
                status.backpressure_count = facts.backpressured;
                status.pending_updates = facts.pending;
                status.update_high_water = facts.high_water;
                status.retained_resources = render->assetStatus().size();
            }
        }
    }
};

SceneRun::SceneRun(process::ExecutionRuntime &runtime, process::TaskScope &tasks, lux::render::RenderRuntime &renderer,
                   SceneEditorMetadata metadata, std::shared_ptr<SceneRunSlot> slot)
    : data_(std::make_unique<Data>(runtime, tasks, renderer, std::move(metadata), std::move(slot)))
{
}
SceneRun::~SceneRun()
{
    assert(settled());
}
const RunStatus &SceneRun::status() const noexcept
{
    return data_->status;
}
double SceneRun::coordinatePageSize() const noexcept
{
    return data_->page_size;
}
SceneObjects *SceneRun::objects() noexcept
{
    return data_->active ? &data_->active->objects : nullptr;
}
lux::scene::SceneInstance *SceneRun::scene() noexcept
{
    return data_->active ? data_->active->scene.get() : nullptr;
}
bool SceneRun::takeCatalogChange() noexcept
{
    return data_->active && std::exchange(data_->active->catalog_dirty, false);
}
editing::EditHistory *SceneRun::history() noexcept
{
    return data_->active ? data_->active->pause_history.get() : nullptr;
}
void SceneRun::invalidateDerived() noexcept
{
    assert(data_->active);
    data_->active->driver.invalidate(*data_->active->scene);
}
bool SceneRun::settled() const noexcept
{
    const auto state = data_->status.state;
    return state == ERunState::IDLE || state == ERunState::FINISHED || state == ERunState::FAILED;
}
EditorResult<void> SceneRun::validateStart(std::chrono::nanoseconds delta) const
{
    const auto &d = *data_;
    if (!settled() || d.slot->owner.value)
    {
        return invalid("run.active", EEditorError::BUSY);
    }
    if (delta.count() <= 0 || delta > std::chrono::seconds(1) || d.next_id == UINT64_MAX)
    {
        return invalid("run.fixed-step", EEditorError::INVALID_ARGUMENT);
    }
    return {};
}
EditorResult<RunId> SceneRun::start(DocumentHandle document, editing::HistoryId history, editing::StateId state,
                                    SceneCapture capture, std::shared_ptr<lux::scene::RenderAssetSource> assets,
                                    std::chrono::nanoseconds delta)
{
    auto &d = *data_;
    const auto admitted = validateStart(delta);
    if (!admitted)
    {
        return lux::cxx::unexpected(admitted.error());
    }
    d.stop = {};
    d.decoding =
        std::make_unique<DecodeTask>(d.runtime, stdexec::then(stdexec::schedule(d.runtime.cpu()),
                                                              DecodeRun{std::move(capture), d.stop.get_token()}));
    d.assets = std::move(assets);
    d.receipt = {};
    d.page_size = 0;
    d.delta = delta;
    d.status = {.id = {document, d.next_id++}, .state = ERunState::PREPARING, .captured_state = state};
    d.slot->owner = history;
    d.decoding->start();
    return d.status.id;
}

namespace
{
EditorResult<void> control(lux::scene::SceneDriver::ControlResult result, std::string domain)
{
    if (!result)
    {
        return lux::cxx::unexpected(EditorFailure{
            result.error() == lux::scene::ESceneControlError::BUSY ? EEditorError::BUSY : EEditorError::INVALID_STATE,
            std::move(domain),
            0,
            {},
            result.error()});
    }
    return {};
}
} // namespace
EditorResult<void> SceneRun::pause(RunId id)
{
    auto &d = *data_;
    if (id != d.status.id)
    {
        return invalid("run.identity", EEditorError::STALE_REQUEST);
    }
    if (!d.active || d.status.state != ERunState::RUNNING)
    {
        return invalid("run.pause");
    }
    auto result = control(d.active->driver.pause(*d.active->scene), "run.pause");
    d.status.pause_pending = d.active->scene->progress().pause_pending;
    return result;
}
EditorResult<void> SceneRun::resume(RunId id)
{
    auto &d = *data_;
    if (id != d.status.id)
    {
        return invalid("run.identity", EEditorError::STALE_REQUEST);
    }
    if (!d.active || d.status.state != ERunState::PAUSED)
    {
        return invalid("run.resume");
    }
    auto result = control(d.active->driver.play(*d.active->scene), "run.resume");
    if (result)
    {
        d.active->endPause();
        d.status.state = ERunState::RUNNING;
    }
    return result;
}
EditorResult<void> SceneRun::step(RunId id)
{
    auto &d = *data_;
    if (id != d.status.id)
    {
        return invalid("run.identity", EEditorError::STALE_REQUEST);
    }
    if (!d.active)
    {
        return invalid("run.step");
    }
    auto result = control(d.active->driver.step(*d.active->scene), "run.step");
    if (result)
    {
        d.active->endPause();
        d.status.state = ERunState::RUNNING;
        d.status.pause_pending = true;
    }
    return result;
}
EditorResult<void> SceneRun::stop(RunId id)
{
    auto &d = *data_;
    if (id != d.status.id)
    {
        return invalid("run.identity", EEditorError::STALE_REQUEST);
    }
    if (!settled())
    {
        d.status.state = ERunState::STOPPING;
        d.stop.request_stop();
        if (d.active)
        {
            d.active->driver.stop(*d.active->scene);
        }
    }
    return {};
}

void SceneRun::poll(PollBudget &turn, bool may_release_world)
{
    auto &d = *data_;
    if (settled())
    {
        return;
    }
    const auto fail = [&](EditorFailure error, ERunPhase phase = ERunPhase::STARTUP) {
        if (d.status.result)
        {
            d.status.result = lux::cxx::unexpected(std::move(error));
            d.status.failed_phase = phase;
        }
        static_cast<void>(stop(d.status.id));
    };
    if (d.decoding && d.decoding->ready())
    {
        auto result = d.decoding->take();
        d.decoding.reset();
        if (!result)
        {
            if (d.status.state != ERunState::STOPPING || result.error().code != EEditorError::CANCELLED)
            {
                fail(std::move(result.error()));
            }
        }
        else if (d.status.state != ERunState::STOPPING)
        {
            d.source = std::make_unique<NativeScene>(std::move(*result));
            auto installed = d.install();
            if (!installed)
            {
                fail(std::move(installed.error()));
            }
        }
    }
    if (d.active && d.status.state != ERunState::STOPPING)
    {
        auto &run = *d.active;
        lux::scene::SceneAdvanceBudget budget{turn.system_calls, turn.document_steps, turn.render_programs,
                                              turn.resource_steps};
        static_cast<void>(run.driver.advance(*run.scene, Clock::now(), budget));
        turn.system_calls = budget.system_calls;
        turn.document_steps = budget.new_steps;
        turn.render_programs = budget.publications;
        turn.resource_steps = budget.resource_steps;
        run.refreshObjects();
        const auto &progress = run.scene->progress();
        d.status.steps = progress.clock.step_index;
        d.status.elapsed = progress.clock.elapsed;
        d.status.completed = {progress.simulation_completed, progress.stable_completed, progress.publication_completed};
        d.status.pause_pending = progress.pause_pending;
        d.status.simulation_work = progress.active_work;
        d.status.longest_advance = progress.longest_call;
        d.status.publication_wait = progress.publication_wait;
        if (!progress.result)
        {
            const auto phase = progress.result.error().phase;
            const auto failed_phase = [phase] {
                switch (phase)
                {
                case lux::scene::ESceneDrivePhase::MAINTENANCE:
                    return ERunPhase::MAINTENANCE;
                case lux::scene::ESceneDrivePhase::SIMULATION:
                    return ERunPhase::SIMULATION;
                case lux::scene::ESceneDrivePhase::DERIVATION:
                    return ERunPhase::DERIVATION;
                case lux::scene::ESceneDrivePhase::STABLE:
                    return ERunPhase::STABLE;
                case lux::scene::ESceneDrivePhase::PUBLICATION:
                    return ERunPhase::PUBLICATION;
                default:
                    return ERunPhase::NONE;
                }
            }();
            std::visit(
                [&](const auto &cause) {
                    fail({EEditorError::EXECUTION_FAILURE, "run.advance", 0, {}, cause}, failed_phase);
                },
                progress.result.error().cause);
        }
        else if (progress.state == lux::scene::ESceneDriveState::PAUSED)
        {
            if (!run.pause_history)
            {
                auto paused = run.beginPause();
                if (!paused)
                {
                    fail(std::move(paused.error()));
                }
            }
            if (d.status.state != ERunState::STOPPING)
            {
                d.status.state = ERunState::PAUSED;
            }
        }
    }
    d.observeRender();
    if (d.status.state != ERunState::STOPPING || !may_release_world)
    {
        return;
    }

    // No borrowed field/Pane callback is active here. CPU destruction does
    // not wait for a View or GPU: their retained leases belong to Runtime.
    if (d.active)
    {
        d.status.retired_updates += d.status.pending_updates;
        d.status.pending_updates = 0;
        d.active.reset();
    }
    if (d.decoding)
    {
        return;
    }
    d.observeRender(); // Persistent failure can arrive after CPU destruction.
    if (d.receipt.status().state != lux::render::ESceneResourceState::RETIRED)
    {
        return;
    }
    d.source.reset();
    d.assets.reset();
    d.status.retained_resources = 0;
    d.status.pause_pending = false;
    d.slot->owner = {};
    d.status.state = d.status.result ? ERunState::FINISHED : ERunState::FAILED;
}

EditorResult<std::unique_ptr<lux::render::RenderView>> SceneRun::openView(RunId id, lux::render::ViewConfig config)
{
    auto &d = *data_;
    if (id != d.status.id)
    {
        return invalid("run.identity", EEditorError::STALE_REQUEST);
    }
    if (!d.active || (d.status.state != ERunState::RUNNING && d.status.state != ERunState::PAUSED))
    {
        return invalid("run.view");
    }
    auto *render = d.active->scene->findSceneSystem<lux::scene::RenderSystem>();
    if (!render)
    {
        return invalid("run.view", EEditorError::MISSING_PROVIDER);
    }
    auto view = render->openView(config);
    if (!view)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "run.view", 0, {}, view.error()});
    }
    return std::move(*view);
}
} // namespace lux::editor::scene::detail

#include <cassert>
#include <lux/engine/editor/detail/DocumentTask.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/editor/scene/detail/SceneObjects.hpp>
#include <lux/engine/editor/scene/detail/SceneRun.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/Scene.hpp>
#include <lux/engine/scene/SceneRenderBinding.hpp>
#include <lux/engine/scene/WorldMaterializer.hpp>
#include <lux/engine/simulation/ecs/ComponentChangeSet.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <random>

namespace lux::editor::scene
{
    RunViewLease::RunViewLease(std::unique_ptr<rendering::RenderView> view, std::shared_ptr<detail::RunViewCount> count)
        : count_(std::move(count)), view_(std::move(view))
    {
        ++count_->value;
    }
    RunViewLease::~RunViewLease()
    {
        reset();
    }
    RunViewLease::RunViewLease(RunViewLease &&) noexcept = default;
    RunViewLease &RunViewLease::operator=(RunViewLease &&other) noexcept
    {
        if (this != &other)
        {
            reset();
            count_ = std::move(other.count_);
            view_ = std::move(other.view_);
        }
        return *this;
    }
    void RunViewLease::reset() noexcept
    {
        // RenderView enforces COMPLETE itself. Decrement only after its release.
        view_.reset();
        if (count_)
        {
            assert(count_->value != 0);
            --count_->value;
            count_.reset();
        }
    }
} // namespace lux::editor::scene

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
                auto source = decodeNativeScene(lux::cxx::SharedBytes<>::fromOwner(bytes, *bytes), stop);
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
            std::unique_ptr<lux::scene::Scene> scene;
            SceneObjects objects;
            lux::task::TaskExecutor executor;
            using MeshChanges = lux::simulation::ecs::ExtractionChangeSet<lux::simulation::ecs::Mesh3D,
                                                                          lux::simulation::ecs::ComponentList<>,
                                                                          lux::simulation::ecs::ComponentList<>>;
            MeshChanges resource_changes;
            std::unique_ptr<editing::EditHistory> pause_history;
            bool derive{};
            bool catalog_dirty{}, hierarchy_dirty{};
            std::vector<lux::simulation::ecs::Entity> changed_entities;
            std::vector<entt::scoped_connection> catalog_connections;
            std::vector<bool> selected_components;
            std::mt19937 identity_random{std::random_device{}()};

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
                    const auto id = objects.identities.object(entity);
                    if (!registry.valid(entity))
                    {
                        if (id.valid())
                        {
                            objects.identities.unbind(entity);
                            std::erase_if(objects.rows, [&](const auto &row) { return row.object == id; });
                            std::erase_if(objects.component_versions,
                                          [&](const auto &row) { return row.object == id; });
                            if (objects.selection.object == id)
                            {
                                objects.selection.object = {};
                            }
                        }
                    }
                    else if (!id.valid())
                    {
                        uuids::uuid_random_generator generate(identity_random);
                        lux::world::WorldObjectId created;
                        do
                        {
                            created = {generate()};
                        } while (!created.valid() || objects.identities.entity(created) != ecs::NullEntity);
                        const bool bound = objects.identities.bind(created, entity);
                        assert(bound);
                        objects.rows.push_back({created, {}, "Runtime object"});
                    }
                }
                changed_entities.clear();
                if (changed)
                {
                    for (auto &row : objects.rows)
                    {
                        const auto entity = objects.identities.entity(row.object);
                        const auto *parent = registry.try_get<ecs::Parent>(entity);
                        row.parent = parent ? objects.identities.object(parent->entity) : lux::world::WorldObjectId{};
                    }
                    std::ranges::sort(objects.rows, lux::world::WorldObjectIdLess{}, &SceneObjectRow::object);
                }
                // The Inspector needs only the selected entity's component directory.
                // No per-step copy or scan of all component values is required.
                const auto selected = objects.identities.entity(objects.selection.object);
                std::size_t index{};
                for (const auto &schema : objects.metadata.components().all())
                {
                    const bool present = selected != ecs::NullEntity && schema.operations.has(registry, selected);
                    changed |= selected_components[index] != present;
                    selected_components[index++] = present;
                }
                catalog_dirty |= changed;
            }

            ActiveRun(std::unique_ptr<lux::scene::Scene> value, const NativeScene &source,
                      const lux::scene::SceneMetaManager &metadata, lux::simulation::ecs::WorldEntityMap identities,
                      lux::task::TaskExecutor tasks)
                : scene(std::move(value)), objects(scene->registry(), source, metadata, std::move(identities)),
                  executor(std::move(tasks))
            {
                auto &registry = scene->registry();
                using Entity = lux::simulation::ecs::Entity;
                using Parent = lux::simulation::ecs::Parent;
                catalog_connections.emplace_back(
                    registry.on_construct<Entity>().connect<&ActiveRun::entityChanged>(*this));
                catalog_connections.emplace_back(
                    registry.on_destroy<Entity>().connect<&ActiveRun::entityChanged>(*this));
                catalog_connections.emplace_back(
                    registry.on_construct<Parent>().connect<&ActiveRun::hierarchyChanged>(*this));
                catalog_connections.emplace_back(
                    registry.on_update<Parent>().connect<&ActiveRun::hierarchyChanged>(*this));
                catalog_connections.emplace_back(
                    registry.on_destroy<Parent>().connect<&ActiveRun::hierarchyChanged>(*this));
                selected_components.resize(metadata.components().all().size());
                using namespace entt::literals;
                resource_changes.attach(scene->registry(), "editor.run.frozen-mesh"_hs,
                                        [](auto &storage)
                                        {
                                            storage.template on_construct<lux::simulation::ecs::Mesh3D>()
                                                .template on_update<lux::simulation::ecs::Mesh3D>()
                                                .template on_destroy<lux::scene::ResolvedMeshResources>()
                                                .template on_update<lux::scene::ResolvedMeshResources>();
                                        });
            }

            ~ActiveRun()
            {
                endPause();
                scene->simulation().stop();
                scene->requestStop();
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
        Data(process::ExecutionRuntime &process, rendering::EditorRenderer &render, SceneEditorMetadata meta,
             std::shared_ptr<SceneRunSlot> run_slot)
            : runtime(process), renderer(render), metadata(std::move(meta)), slot(std::move(run_slot))
        {
        }

        process::ExecutionRuntime &runtime;
        rendering::EditorRenderer &renderer;
        SceneEditorMetadata metadata;
        std::shared_ptr<SceneRunSlot> slot;
        RunStatus status;
        std::uint64_t next_id{1};
        std::chrono::nanoseconds delta;
        std::stop_source stop;
        std::shared_ptr<RunViewCount> views = std::make_shared<RunViewCount>();
        SceneResourcePins pins;
        std::unique_ptr<DecodeTask> decoding;
        std::unique_ptr<NativeScene> source;
        std::unique_ptr<lux::scene::SceneRenderBinding> binding;
        std::unique_ptr<ActiveRun> active;
        Clock::time_point next{}, waiting_at{};
        bool publishing{}, step_requested{};
        double page_size{1024};

        EditorResult<void> install(lux::scene::SceneRenderInput *input)
        {
            const auto world =
                std::shared_ptr<const lux::world::WorldDescription>(source->world, &source->world->data());
            std::vector<lux::scene::SceneCapabilityProvider> providers;
            if (input)
            {
                providers.push_back(lux::scene::makeSceneCapabilityProvider<lux::scene::SceneRenderInput>(
                    "main-window", "lux.render.input", *input));
            }
            auto created = lux::scene::Scene::create(
                {std::shared_ptr<const lux::scene::SceneDescription>(source->scene, &source->scene->data()), world,
                 std::shared_ptr<const lux::simulation::SimulationDescription>(source->simulation,
                                                                               &source->simulation->data()),
                 *metadata.scene, providers, lux::simulation::ESimulationMode::EVOLUTION});
            if (!created)
            {
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::SOURCE_FAILURE, "run.scene.create", 0, {}, created.error()});
            }
            auto materializer = lux::scene::WorldMaterializer::create(world, metadata.scene->components());
            if (!materializer)
            {
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::SOURCE_FAILURE, "run.schemas", 0, {}, materializer.error()});
            }
            std::vector<lux::world::WorldPartitionObjectView> objects;
            for (const auto &partition : source->partitions)
            {
                for (std::size_t i{}; i < partition.objectCount(); ++i)
                {
                    objects.push_back(partition.objectAt(i));
                }
            }
            lux::simulation::ecs::WorldEntityMap identities;
            auto materialized = materializer->objects((*created)->registry(), identities, objects);
            if (!materialized)
            {
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::SOURCE_FAILURE, "run.materialize", 0, {}, materialized.error()});
            }
            for (const auto &resource : pins.values())
            {
                const auto entity = identities.entity(resource.object);
                if (entity == lux::simulation::ecs::NullEntity)
                {
                    return invalid("run.resource.identity");
                }
                (*created)->registry().emplace<lux::scene::ResolvedMeshResources>(entity, resource.value);
            }
            auto sealed = (*created)->simulation().seal();
            if (!sealed)
            {
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::SOURCE_FAILURE, "run.seal", 0, {}, sealed.error()});
            }
            auto executor = lux::task::TaskExecutor::create({0, 1024});
            if (!executor)
            {
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::EXECUTION_FAILURE, "run.executor", 0, {}, executor.error()});
            }
            active = std::make_unique<ActiveRun>(std::move(*created), *source, *metadata.scene, std::move(identities),
                                                 std::move(*executor));
            status.state = ERunState::RUNNING;
            next = Clock::now();
            return {};
        }
    };

    SceneRun::SceneRun(process::ExecutionRuntime &runtime, rendering::EditorRenderer &renderer,
                       SceneEditorMetadata metadata, std::shared_ptr<SceneRunSlot> slot)
        : data_(std::make_unique<Data>(runtime, renderer, std::move(metadata), std::move(slot)))
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
        data_->active->derive = true;
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
                                        SceneCapture capture, SceneResourcePins pins, std::chrono::nanoseconds delta)
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
        d.pins = std::move(pins);
        d.delta = delta;
        d.status = {.id = {document, d.next_id++}, .state = ERunState::PREPARING, .captured_state = state};
        d.status.retained_resources = d.pins.values().size();
        d.slot->owner = history;
        d.publishing = false;
        d.step_requested = false;
        d.decoding->start();
        return d.status.id;
    }

    EditorResult<void> SceneRun::pause(RunId id)
    {
        auto &d = *data_;
        if (id != d.status.id)
        {
            return invalid("run.identity", EEditorError::STALE_REQUEST);
        }
        if (d.status.state != ERunState::RUNNING)
        {
            return invalid("run.pause");
        }
        d.status.pause_pending = true;
        return {};
    }
    EditorResult<void> SceneRun::resume(RunId id)
    {
        auto &d = *data_;
        if (id != d.status.id)
        {
            return invalid("run.identity", EEditorError::STALE_REQUEST);
        }
        if (d.status.state != ERunState::PAUSED || d.step_requested)
        {
            return invalid("run.resume");
        }
        d.active->endPause();
        d.status.state = ERunState::RUNNING;
        d.next = Clock::now();
        return {};
    }
    EditorResult<void> SceneRun::step(RunId id)
    {
        auto &d = *data_;
        if (id != d.status.id)
        {
            return invalid("run.identity", EEditorError::STALE_REQUEST);
        }
        if (d.step_requested)
        {
            return invalid("run.step.pending", EEditorError::BUSY);
        }
        if (d.status.state != ERunState::PAUSED || d.step_requested)
        {
            return invalid("run.step");
        }
        d.active->endPause();
        d.step_requested = true;
        d.status.state = ERunState::RUNNING;
        d.status.pause_pending = true;
        return {};
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
        }
        return {};
    }

    void SceneRun::poll(PollBudget &turn, bool may_release_world)
    {
        auto &d = *data_;
        auto &budget = turn.render_programs;
        if (settled())
        {
            return;
        }
        const auto fail = [&](EditorFailure error, ERunPhase phase = ERunPhase::STARTUP)
        {
            if (d.status.result)
            {
                d.status.result = lux::cxx::unexpected(std::move(error));
                d.status.failed_phase = phase;
            }
            static_cast<void>(stop(d.status.id));
        };
        const auto poll_binding = [&]
        {
            if (!d.binding)
            {
                return;
            }
            const auto consumed = d.binding->poll(budget);
            assert(consumed <= budget);
            budget -= consumed;
            const auto facts = d.binding->statistics();
            d.status.published_updates = facts.published;
            d.status.forwarded_updates = facts.forwarded;
            d.status.retired_updates = facts.retired_unforwarded;
            d.status.backpressure_count = facts.backpressured;
            d.status.pending_updates = facts.pending;
            d.status.update_high_water = facts.high_water;
            d.status.render_drain_submitted = d.binding->drainSubmitted();
            if (d.status.result && d.binding->hasFailure())
            {
                fail({EEditorError::SOURCE_FAILURE, "run.render", 0, {}, d.binding->failure()},
                     d.status.render_scene.isValid() ? ERunPhase::PUBLICATION : ERunPhase::STARTUP);
            }
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
                auto bound = beginSceneRendering(d.renderer, *d.source, d.metadata);
                if (!bound)
                {
                    fail(std::move(bound.error()));
                }
                else
                {
                    d.binding = std::move(*bound);
                }
            }
        }
        poll_binding();
        if (d.status.state == ERunState::PREPARING && d.source && !d.active)
        {
            if (!d.binding)
            {
                auto installed = d.install(nullptr);
                if (!installed)
                {
                    fail(std::move(installed.error()));
                }
            }
            else if (d.binding->state() == lux::scene::ESceneRenderBindingState::READY)
            {
                auto input = d.binding->takeInput();
                if (!input)
                {
                    fail({EEditorError::SOURCE_FAILURE, "run.input", 0, {}, input.error()});
                }
                else
                {
                    d.status.render_scene = input->sceneId();
                    d.page_size = input->coordinatePageSize();
                    auto installed = d.install(&*input);
                    if (!installed)
                    {
                        fail(std::move(installed.error()));
                    }
                }
            }
        }
        if (d.active && d.status.state != ERunState::STOPPING)
        {
            auto &run = *d.active;
            auto *render = run.scene->findSceneSystem<lux::scene::RenderSystem>();
            if (d.publishing)
            {
                if (render && render->lastPublishResult() == lux::scene::ERenderPublishResult::BACKPRESSURED)
                {
                    if (render->tryPublish() == lux::scene::ERenderPublishResult::FAILED)
                    {
                        fail({EEditorError::EXECUTION_FAILURE, "run.publish"}, ERunPhase::PUBLICATION);
                    }
                }
                if ((!d.binding || !d.binding->hasPendingUpdate()) &&
                    (!render || render->lastPublishResult() != lux::scene::ERenderPublishResult::BACKPRESSURED))
                {
                    d.publishing = false;
                    d.status.publication_wait += Clock::now() - d.waiting_at;
                    d.status.completed.publication = d.status.steps;
                }
            }
            if (!d.publishing && d.status.state != ERunState::STOPPING)
            {
                if (d.status.pause_pending && !d.step_requested)
                {
                    auto paused = run.beginPause();
                    if (!paused)
                    {
                        fail(std::move(paused.error()));
                    }
                    else
                    {
                        d.status.state = ERunState::PAUSED;
                        d.status.pause_pending = false;
                    }
                }
                const bool refresh = d.status.state == ERunState::PAUSED && run.derive;
                const bool advance =
                    d.status.state == ERunState::RUNNING && (d.step_requested || Clock::now() >= d.next);
                if ((advance || refresh) && turn.document_steps)
                {
                    --turn.document_steps;
                    const auto started = Clock::now();
                    auto evolved = advance ? run.scene->simulation().execute(run.executor, d.delta)
                                           : run.scene->simulation().refresh(run.executor);
                    const auto clock = run.scene->simulation().clock().snapshot();
                    run.refreshObjects();
                    d.status.steps = clock.step_index;
                    d.status.elapsed = clock.elapsed;
                    d.step_requested = false;
                    if (!evolved)
                    {
                        fail({EEditorError::EXECUTION_FAILURE, "run.simulation", 0, {}, evolved.error()},
                             ERunPhase::SIMULATION);
                    }
                    else
                    {
                        if (advance)
                        {
                            d.status.completed.simulation = d.status.steps;
                        }
                        for (const auto entity : run.resource_changes.view())
                        {
                            const auto &visual = run.scene->registry().get<lux::simulation::ecs::Mesh3D>(entity).value;
                            const auto *resolved =
                                run.scene->registry().try_get<lux::scene::ResolvedMeshResources>(entity);
                            if (!resolved || visual.mesh != resolved->mesh_source ||
                                visual.material != resolved->material_source)
                            {
                                fail({EEditorError::INVALID_STATE, "run.frozen-resources",
                                      lux::simulation::ecs::entityBits(entity),
                                      "Run requested a mesh or material outside its frozen resource binding"},
                                     ERunPhase::RESOURCES);
                                break;
                            }
                        }
                        run.resource_changes.clear();
                        if (d.status.state != ERunState::STOPPING)
                        {
                            auto stable = run.scene->executeStablePoint();
                            if (!stable)
                            {
                                fail({EEditorError::EXECUTION_FAILURE, "run.stable", 0, {}, stable.error()},
                                     ERunPhase::STABLE);
                            }
                            else
                            {
                                run.derive = false;
                                d.status.completed.stable = d.status.steps;
                                d.publishing = true;
                                d.waiting_at = Clock::now();
                            }
                        }
                    }
                    const auto finished = Clock::now();
                    d.status.simulation_work += finished - started;
                    d.next = std::max(started + d.delta, finished);
                }
            }
        }
        if (d.status.state != ERunState::STOPPING)
        {
            return;
        }
        if (!may_release_world)
        {
            return;
        }
        // No queued worker owns the World. All final clock/failure facts are already on Main.
        d.active.reset();
        if (d.decoding || d.views->value != 0)
        {
            return;
        }
        if (d.binding)
        {
            d.binding->requestClose();
            poll_binding();
            if (d.binding->state() != lux::scene::ESceneRenderBindingState::CLOSED)
            {
                return;
            }
            d.binding.reset();
        }
        d.source.reset();
        d.pins = {};
        d.status.retained_resources = 0;
        d.status.pause_pending = false;
        d.slot->owner = {};
        d.status.state = d.status.result ? ERunState::FINISHED : ERunState::FAILED;
    }

    EditorResult<RunViewLease> SceneRun::openView(RunId id, rendering::ViewConfig config)
    {
        auto &d = *data_;
        if (id != d.status.id)
        {
            return invalid("run.identity", EEditorError::STALE_REQUEST);
        }
        if ((d.status.state != ERunState::RUNNING && d.status.state != ERunState::PAUSED) ||
            !d.status.render_scene.isValid())
        {
            return invalid("run.view");
        }
        config.coordinate_page_size = d.page_size;
        auto view = d.renderer.openView(d.status.render_scene, config);
        if (!view)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "run.view", 0, {}, view.error()});
        }
        return RunViewLease(std::move(*view), d.views);
    }
} // namespace lux::editor::scene::detail

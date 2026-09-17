#include <cassert>
#include <condition_variable>
#include <limits>
#include <lux/engine/editor/detail/DocumentTask.hpp>
#include <lux/engine/editor/scene/detail/SceneRun.hpp>
#include <lux/engine/scene/LatestSpscExchange.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/Scene.hpp>
#include <lux/engine/scene/SceneRenderBinding.hpp>
#include <lux/engine/scene/WorldMaterializer.hpp>
#include <lux/engine/simulation/ecs/ComponentChangeSet.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <mutex>

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
        auto invalid(std::string domain, EEditorError code = EEditorError::INVALID_STATE)
        {
            return lux::cxx::unexpected(EditorFailure{code, std::move(domain)});
        }

        struct RunObservation final
        {
            std::uint64_t steps{};
            std::chrono::nanoseconds elapsed{};
            bool paused{};
            RunCompletedPhases completed;
            std::chrono::nanoseconds work{}, waiting{};
        };
        struct RunCompletion final
        {
            RunObservation observation;
            ERunPhase failed_phase{ERunPhase::NONE};
            EditorResult<void> result;
        };

        struct RunControl final
        {
            std::mutex mutex;
            std::condition_variable_any changed;
            std::stop_source stop;
            bool paused{};
            bool step_requested{};
            bool step_inflight{};
            lux::scene::LatestSpscExchange<RunObservation> observation;
        };

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

        struct ExecuteRun final
        {
            NativeScene source;
            std::shared_ptr<const lux::scene::SceneMetaManager> metadata;
            lux::scene::SceneRenderInput input;
            std::vector<FrozenSceneResource> resources;
            std::shared_ptr<RunControl> control;
            std::chrono::nanoseconds delta;

            EditorResult<RunCompletion> operator()() noexcept
            {
                RunObservation observed;
                ERunPhase phase = ERunPhase::STARTUP;
                auto working_at = std::chrono::steady_clock::now();
                bool working = false;
                const auto accumulate = [&]
                {
                    if (working)
                    {
                        observed.work += std::chrono::steady_clock::now() - working_at;
                        working = false;
                    }
                };
                const auto failed = [&](EditorFailure error) -> EditorResult<RunCompletion>
                {
                    accumulate();
                    return RunCompletion{observed, phase, lux::cxx::unexpected(std::move(error))};
                };
                const auto stop = control->stop.get_token();
                if (stop.stop_requested())
                {
                    return RunCompletion{};
                }
                const auto world =
                    std::shared_ptr<const lux::world::WorldDescription>(source.world, &source.world->data());
                const std::array providers{lux::scene::makeSceneCapabilityProvider<lux::scene::SceneRenderInput>(
                    "main-window", "lux.render.input", input)};
                auto created = lux::scene::Scene::create(
                    {std::shared_ptr<const lux::scene::SceneDescription>(source.scene, &source.scene->data()), world,
                     std::shared_ptr<const lux::simulation::SimulationDescription>(source.simulation,
                                                                                   &source.simulation->data()),
                     *metadata, providers, lux::simulation::ESimulationMode::EVOLUTION});
                if (!created)
                {
                    return failed(EditorFailure{EEditorError::SOURCE_FAILURE,
                                                "run.scene.create",
                                                static_cast<std::uint64_t>(created.error().code),
                                                {},
                                                created.error()});
                }
                // Every exit below destroys the real Scene on this worker,
                // including partial startup.
                auto scene = std::move(*created);
                auto materializer = lux::scene::WorldMaterializer::create(world, metadata->components());
                if (!materializer)
                {
                    return failed(EditorFailure{EEditorError::SOURCE_FAILURE,
                                                "run.schemas",
                                                static_cast<std::uint64_t>(materializer.error().code),
                                                {},
                                                materializer.error()});
                }
                std::vector<lux::world::WorldPartitionObjectView> objects;
                for (const auto &partition : source.partitions)
                {
                    for (std::size_t i{}; i < partition.objectCount(); ++i)
                    {
                        objects.push_back(partition.objectAt(i));
                    }
                }
                lux::simulation::ecs::WorldEntityMap identities;
                auto materialized = materializer->objects(scene->registry(), identities, objects);
                if (!materialized)
                {
                    return failed(EditorFailure{EEditorError::SOURCE_FAILURE,
                                                "run.materialize",
                                                static_cast<std::uint64_t>(materialized.error().code),
                                                {},
                                                materialized.error()});
                }
                for (const auto &resource : resources)
                {
                    const auto entity = identities.entity(resource.object);
                    if (entity == lux::simulation::ecs::NullEntity)
                    {
                        return failed({EEditorError::INVALID_STATE, "run.resource.identity"});
                    }
                    scene->registry().emplace<lux::scene::ResolvedMeshResources>(entity, resource.value);
                }
                // Check only changed mesh references. The frozen resource set is
                // not a streaming service.
                using MeshChanges = lux::simulation::ecs::ExtractionChangeSet<lux::simulation::ecs::Mesh3D,
                                                                              lux::simulation::ecs::ComponentList<>,
                                                                              lux::simulation::ecs::ComponentList<>>;
                MeshChanges resource_changes;
                using namespace entt::literals;
                resource_changes.attach(scene->registry(), "editor.run.frozen-mesh"_hs,
                                        [](auto &storage)
                                        {
                                            storage.template on_construct<lux::simulation::ecs::Mesh3D>()
                                                .template on_update<lux::simulation::ecs::Mesh3D>()
                                                .template on_destroy<lux::scene::ResolvedMeshResources>()
                                                .template on_update<lux::scene::ResolvedMeshResources>();
                                        });
                auto sealed = scene->simulation().seal();
                if (!sealed)
                {
                    return failed(EditorFailure{EEditorError::SOURCE_FAILURE,
                                                "run.seal",
                                                static_cast<std::uint64_t>(sealed.error().code),
                                                {},
                                                sealed.error()});
                }
                auto executor = lux::task::TaskExecutor::create({0, 1024});
                if (!executor)
                {
                    return failed(EditorFailure{EEditorError::EXECUTION_FAILURE,
                                                "run.executor",
                                                static_cast<std::uint64_t>(executor.error().code),
                                                {},
                                                executor.error()});
                }
                auto *render = scene->findSceneSystem<lux::scene::RenderSystem>();
                auto next = std::chrono::steady_clock::now();
                while (!stop.stop_requested())
                {
                    {
                        std::unique_lock lock(control->mutex);
                        observed.paused = control->paused;
                        control->observation.write() = observed;
                        control->observation.publish();
                        if (control->paused)
                        {
                            control->changed.wait(lock, stop,
                                                  [&] { return !control->paused || control->step_requested; });
                            next = std::chrono::steady_clock::now();
                        }
                        else
                        {
                            control->changed.wait_until(lock, stop, next, [&] { return control->paused; });
                        }
                        if (stop.stop_requested())
                        {
                            break;
                        }
                        if (control->paused && !control->step_requested)
                        {
                            continue;
                        }
                        control->step_inflight = control->step_requested;
                        control->step_requested = false;
                    }
                    const auto step_started = std::chrono::steady_clock::now();
                    working_at = step_started;
                    working = true;
                    phase = ERunPhase::SIMULATION;
                    auto evolved = scene->simulation().execute(*executor, delta);
                    const auto clock = scene->simulation().clock().snapshot();
                    observed.steps = clock.step_index;
                    observed.elapsed = clock.elapsed;
                    if (!evolved)
                    {
                        return failed(EditorFailure{EEditorError::EXECUTION_FAILURE,
                                                    "run.simulation",
                                                    static_cast<std::uint64_t>(evolved.error().code),
                                                    {},
                                                    evolved.error()});
                    }
                    observed.completed.simulation = observed.steps;
                    phase = ERunPhase::RESOURCES;
                    for (const auto entity : resource_changes.view())
                    {
                        const auto &visual = scene->registry().get<lux::simulation::ecs::Mesh3D>(entity).value;
                        const auto *resolved = scene->registry().try_get<lux::scene::ResolvedMeshResources>(entity);
                        if (!resolved || visual.mesh != resolved->mesh_source ||
                            visual.material != resolved->material_source)
                        {
                            return failed(EditorFailure{EEditorError::INVALID_STATE, "run.frozen-resources",
                                                        lux::simulation::ecs::entityBits(entity),
                                                        "Run requested a mesh or material outside its frozen "
                                                        "resource binding"});
                        }
                    }
                    resource_changes.clear();
                    phase = ERunPhase::STABLE;
                    auto stable = scene->executeStablePoint();
                    if (!stable)
                    {
                        return failed(EditorFailure{EEditorError::EXECUTION_FAILURE,
                                                    "run.stable",
                                                    static_cast<std::uint64_t>(stable.error().code),
                                                    {},
                                                    stable.error()});
                    }
                    observed.completed.stable = observed.steps;
                    phase = ERunPhase::PUBLICATION;
                    while (render && render->lastPublishResult() == lux::scene::ERenderPublishResult::BACKPRESSURED)
                    {
                        const auto waiting_at = std::chrono::steady_clock::now();
                        accumulate();
                        const bool capacity = render->waitForCapacity(stop);
                        working_at = std::chrono::steady_clock::now();
                        working = true;
                        observed.waiting += working_at - waiting_at;
                        if (!capacity)
                        {
                            if (!stop.stop_requested())
                            {
                                return failed({EEditorError::EXECUTION_FAILURE, "run.publish.stopping"});
                            }
                            break;
                        }
                        if (render->tryPublish() == lux::scene::ERenderPublishResult::FAILED)
                        {
                            return failed(EditorFailure{
                                EEditorError::EXECUTION_FAILURE,
                                "run.publish",
                                0,
                                {},
                                lux::scene::SceneExecutionFailure{lux::scene::ESceneExecutionError::SYSTEM_FAILURE,
                                                                  render->instanceId()}});
                        }
                    }
                    if (stop.stop_requested())
                    {
                        break;
                    }
                    observed.completed.publication = observed.steps;
                    phase = ERunPhase::PRESENTATION;
                    auto presentation = scene->executePresentation();
                    if (!presentation)
                    {
                        return failed(EditorFailure{EEditorError::EXECUTION_FAILURE,
                                                    "run.presentation",
                                                    static_cast<std::uint64_t>(presentation.error().code),
                                                    {},
                                                    presentation.error()});
                    }
                    observed.completed.presentation = observed.steps;
                    accumulate();
                    // Do not catch up by advancing extra unpresented simulation
                    // steps.
                    {
                        std::lock_guard lock(control->mutex);
                        control->step_inflight = false;
                    }
                    // Fixed simulation dt, paced by a wall-clock deadline. Work is
                    // part of the period. If late, discard pacing debt and
                    // re-anchor; never execute a burst of catch-up steps or change
                    // simulation dt.
                    next = step_started + delta;
                    const auto completed_at = std::chrono::steady_clock::now();
                    if (next < completed_at)
                    {
                        next = completed_at;
                    }
                }
                accumulate();
                scene->simulation().stop();
                scene->requestStop();
                resource_changes.detach();
                scene.reset();
                return RunCompletion{observed, ERunPhase::NONE, {}};
            }
        };
        using ExecuteTask = lux::editor::detail::ScheduledDocumentTask<process::CpuScheduler, ExecuteRun>;
    } // namespace

    struct SceneRun::Data final
    {
        Data(process::ExecutionRuntime &runtime, rendering::EditorRenderer &renderer,
             std::shared_ptr<const lux::scene::SceneMetaManager> metadata, std::shared_ptr<SceneRunSlot> slot)
            : runtime(runtime), renderer(renderer), metadata(std::move(metadata)), slot(std::move(slot))
        {
        }
        process::ExecutionRuntime &runtime;
        rendering::EditorRenderer &renderer;
        std::shared_ptr<const lux::scene::SceneMetaManager> metadata;
        std::shared_ptr<SceneRunSlot> slot;
        RunStatus status;
        std::uint64_t next_id{1};
        std::chrono::nanoseconds delta;
        std::shared_ptr<RunControl> control;
        std::shared_ptr<RunViewCount> views = std::make_shared<RunViewCount>();
        SceneResourcePins pins;
        std::unique_ptr<DecodeTask> decoding;
        std::unique_ptr<NativeScene> source;
        std::unique_ptr<lux::scene::SceneRenderBinding> binding;
        std::unique_ptr<ExecuteTask> executing;
        lux::render::RenderSceneId scene;
        double page_size{1024};
    };

    SceneRun::SceneRun(process::ExecutionRuntime &runtime, rendering::EditorRenderer &renderer,
                       std::shared_ptr<const lux::scene::SceneMetaManager> metadata, std::shared_ptr<SceneRunSlot> slot)
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
    bool SceneRun::settled() const noexcept
    {
        const auto state = data_->status.state;
        return state == ERunState::IDLE || state == ERunState::FINISHED || state == ERunState::FAILED;
    }
    double SceneRun::coordinatePageSize() const noexcept
    {
        return data_->page_size;
    }

    EditorResult<void> SceneRun::validateStart(std::chrono::nanoseconds delta) const
    {
        const auto &d = *data_;
        if (!settled() || d.slot->owner.value)
        {
            return invalid("run.active", EEditorError::BUSY);
        }
        if (d.runtime.cpuConcurrency() < 2)
        {
            return invalid("run.cpu-capacity", EEditorError::CAPACITY);
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
        d.control = std::make_shared<RunControl>();
        auto task = std::make_unique<DecodeTask>(
            d.runtime, stdexec::then(stdexec::schedule(d.runtime.cpu()),
                                     DecodeRun{std::move(capture), d.control->stop.get_token()}));
        d.pins = std::move(pins);
        d.delta = delta;
        d.status = {.id = {document, d.next_id++}, .state = ERunState::PREPARING, .captured_state = state};
        d.status.retained_resources = d.pins.values().size();
        d.slot->owner = history;

        d.scene = {};
        d.decoding = std::move(task);
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
        std::lock_guard lock(d.control->mutex);
        d.control->paused = true;
        d.status.pause_pending = true;
        d.control->changed.notify_all();
        return {};
    }
    EditorResult<void> SceneRun::resume(RunId id)
    {
        auto &d = *data_;
        if (id != d.status.id)
        {
            return invalid("run.identity", EEditorError::STALE_REQUEST);
        }
        if (d.status.state != ERunState::PAUSED)
        {
            return invalid("run.resume");
        }
        std::lock_guard lock(d.control->mutex);
        d.control->paused = false;
        d.control->step_requested = false;
        d.control->changed.notify_all();
        return {};
    }
    EditorResult<void> SceneRun::step(RunId id)
    {
        auto &d = *data_;
        if (id != d.status.id)
        {
            return invalid("run.identity", EEditorError::STALE_REQUEST);
        }
        if (d.status.state != ERunState::PAUSED)
        {
            return invalid("run.step");
        }
        std::lock_guard lock(d.control->mutex);
        if (!d.control->paused || d.control->step_requested || d.control->step_inflight)
        {
            return invalid("run.step.pending", EEditorError::BUSY);
        }
        d.control->step_requested = true;
        d.control->changed.notify_all();
        return {};
    }
    EditorResult<void> SceneRun::stop(RunId id)
    {
        auto &d = *data_;
        if (id != d.status.id)
        {
            return invalid("run.identity", EEditorError::STALE_REQUEST);
        }
        if (settled())
        {
            return {};
        }
        d.status.state = ERunState::STOPPING;
        d.control->stop.request_stop();
        d.control->changed.notify_all();
        return {};
    }

    void SceneRun::poll(std::size_t budget)
    {
        auto &d = *data_;
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
        const auto observe_transport = [&]
        {
            const auto transport = d.binding->statistics();
            d.status.published_updates = transport.published;
            d.status.forwarded_updates = transport.forwarded;
            d.status.retired_updates = transport.retired_unforwarded;
            d.status.backpressure_count = transport.backpressured;
            d.status.pending_updates = transport.pending;
            d.status.update_high_water = transport.high_water;
            d.status.render_drain_submitted = d.binding->drainSubmitted();
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
                const auto &description = d.source->scene->data();
                for (std::size_t i{}; i < description.systemCount(); ++i)
                {
                    const auto system = description.systemAt(i);
                    if (system.type() != lux::scene::builtinRenderSystemRegistration().type)
                    {
                        continue;
                    }
                    auto bound = lux::scene::SceneRenderBinding::begin(d.renderer, system, d.metadata);
                    if (!bound)
                    {
                        fail({EEditorError::SOURCE_FAILURE, "run.render.begin", 0, {}, bound.error()});
                    }
                    else
                    {
                        d.binding = std::move(*bound);
                    }
                    break;
                }
                if (!d.binding && d.status.state != ERunState::STOPPING)
                {
                    fail({EEditorError::MISSING_PROVIDER, "run.render", 0, "The first Run requires a RenderSystem"});
                }
            }
        }
        if (d.binding)
        {
            d.binding->poll(budget);
            observe_transport();
            if (d.binding->state() == lux::scene::ESceneRenderBindingState::FAILED)
            {
                fail({EEditorError::SOURCE_FAILURE, "run.render", 0, {}, d.binding->failure()},
                     d.executing ? ERunPhase::PUBLICATION : ERunPhase::STARTUP);
            }
            if (d.status.state == ERunState::PREPARING && !d.executing &&
                d.binding->state() == lux::scene::ESceneRenderBindingState::READY)
            {
                auto input = d.binding->takeInput();
                if (!input)
                {
                    fail({EEditorError::SOURCE_FAILURE, "run.input", 0, {}, input.error()});
                }
                else
                {
                    d.scene = input->sceneId();
                    d.status.render_scene = d.scene;
                    d.page_size = input->coordinatePageSize();
                    std::vector<FrozenSceneResource> resources(d.pins.values().begin(), d.pins.values().end());
                    d.executing = std::make_unique<ExecuteTask>(
                        d.runtime, stdexec::then(stdexec::schedule(d.runtime.cpu()),
                                                 ExecuteRun{std::move(*d.source), d.metadata, std::move(*input),
                                                            std::move(resources), d.control, d.delta}));
                    d.source.reset();
                    d.executing->start();
                }
            }
        }
        if (d.executing && d.control->observation.acquireLatest())
        {
            const auto &observed = d.control->observation.read();
            d.status.completed = observed.completed;
            d.status.steps = observed.steps;
            d.status.elapsed = observed.elapsed;
            d.status.simulation_work = observed.work;
            d.status.publication_wait = observed.waiting;
            if (d.status.state != ERunState::STOPPING)
            {
                d.status.state = observed.paused ? ERunState::PAUSED : ERunState::RUNNING;
                if (observed.paused)
                {
                    d.status.pause_pending = false;
                }
            }
        }
        if (d.executing && d.executing->ready())
        {
            auto result = d.executing->take();
            d.executing.reset();

            if (!result)
            {
                fail(std::move(result.error()));
            }
            else
            {
                const auto &observed = result->observation;
                d.status.steps = observed.steps;
                d.status.elapsed = observed.elapsed;
                d.status.completed = observed.completed;
                d.status.simulation_work = observed.work;
                d.status.publication_wait = observed.waiting;
                if (!result->result)
                {
                    fail(std::move(result->result.error()), result->failed_phase);
                }
                else
                {
                    static_cast<void>(stop(d.status.id));
                }
            }
        }
        if (d.status.state != ERunState::STOPPING || d.decoding || d.executing || d.views->value != 0)
        {
            return;
        }
        if (d.binding)
        {
            d.binding->requestClose();
            d.binding->poll(budget);
            observe_transport();
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
        if (d.status.state != ERunState::RUNNING && d.status.state != ERunState::PAUSED)
        {
            return invalid("run.view");
        }
        config.coordinate_page_size = d.page_size;
        auto view = d.renderer.openView(d.scene, config);
        if (!view)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "run.view", 0, {}, view.error()});
        }
        return RunViewLease(std::move(*view), d.views);
    }
} // namespace lux::editor::scene::detail

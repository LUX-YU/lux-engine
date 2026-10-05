#include <lux/engine/editor/editing/EditExecutor.hpp>
#include <lux/engine/editor/scene/RunStore.hpp>
#include <lux/engine/editor/scene/SceneSession.hpp>
#include "RunDebugEdits.hpp"
#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/process/world_loading/WorldMemoryStorageSource.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/ScriptRuntimeSystem.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <lux/cxx/container/SlotMap.hpp>
#include <atomic>
#include <thread>

namespace lux::editor::scene
{
    namespace
    {
        template <class Error> auto rejected(Error error)
        {
            return lux::cxx::unexpected(RunFailure{std::move(error)});
        }
        struct DispatchScope final
        {
            bool& active;
            bool previous;
            explicit DispatchScope(bool& value) noexcept : active(value), previous(value)
            {
                active = true;
            }
            ~DispatchScope()
            {
                active = previous;
            }
        };
        constexpr editing::HistoryLimits historyLimits{1024, 64U * 1024U * 1024U, 16U * 1024U * 1024U, 256};
        std::atomic_uint64_t nextDomain{1};
    }

    class RunSession final
    {
    public:
        RunEnvironment environment;
        std::shared_ptr<const SceneSnapshot> snapshot;
        lux::scene::ScenePackage package;
        lux::scene::SceneInstanceLease instance;
        RunInfo info;
        lux::scene::InstanceRetirement retirement;
        std::unique_ptr<RunDebugEdits> debug;
        std::vector<entt::scoped_connection> connections;
        std::vector<lux::scene::SceneStepTicket> steps;
        void structureChanged(simulation::ecs::Registry&, simulation::ecs::Entity) noexcept
        {
            ++info.structure_revision;
        }
        void observe(simulation::ecs::Registry& registry)
        {
            namespace ecs = simulation::ecs;
            connections.emplace_back(registry.on_construct<ecs::Entity>().connect<&RunSession::structureChanged>(*this)
            );
            connections.emplace_back(registry.on_destroy<ecs::Entity>().connect<&RunSession::structureChanged>(*this));
            connections.emplace_back(registry.on_construct<ecs::Parent>().connect<&RunSession::structureChanged>(*this)
            );
            connections.emplace_back(registry.on_update<ecs::Parent>().connect<&RunSession::structureChanged>(*this));
            connections.emplace_back(registry.on_destroy<ecs::Parent>().connect<&RunSession::structureChanged>(*this));
            ++info.structure_revision; // Existing entities are included by the first directory read.
        }
        void observeRender(const simulation::ecs::Registry& registry)
        {
            const auto* render = lux::scene::RenderSceneState::find(registry, info.provenance.configuration.viewport);
            if (!render)
                return;
            const auto& facts = render->transport;
            info.coordinate_page_size = render->coordinate_page_size;
            if (environment.resources)
                info.render_scene = environment.resources->sceneReceipt(render->resource).status().scene;
            info.published_updates = facts.published;
            info.forwarded_updates = facts.forwarded;
            info.retired_updates = facts.retired_unforwarded;
            info.backpressure_count = facts.backpressured;
            info.pending_updates = facts.pending;
            info.update_high_water = facts.high_water;
            if (const auto* assets = lux::scene::RenderAssets::find(registry, render->system))
                info.retained_resources = assets->statuses().size();
        }

        [[nodiscard]] RunResult<void> finishEditing()
        {
            if (debug)
            {
                const auto view = debug->history->view();
                if (!view)
                    return rejected(view.error());
                const bool can_finish =
                    view->phase == editing::EHistoryPhase::IDLE || view->phase == editing::EHistoryPhase::CLOSED;
                if (!can_finish)
                    return rejected(ERunError::BUSY);
                auto result = debug->editing.finishFieldEdits();
                if (!result)
                    return rejected(result.error());
            }
            return {};
        }
        [[nodiscard]] RunResult<void> closeDebug()
        {
            auto finished = finishEditing();
            if (!finished)
                return finished;
            if (debug)
            {
                const auto closed = editing::EditExecutor{}.close(*debug->history);
                if (!closed)
                    return rejected(closed.error());
                debug.reset();
            }
            return {};
        }
    };

    struct RunStore::Impl final
    {
        lux::scene::SceneRuntime& runtime;
        process::ExecutionRuntime& execution;
        std::shared_ptr<process::TaskScope> tasks;
        const std::uint64_t domain{nextDomain.fetch_add(1, std::memory_order_relaxed)};
        const std::thread::id owner{std::this_thread::get_id()};
        const std::size_t capacity;
        std::uint64_t next_start{1};
        lux::cxx::SlotMap<std::unique_ptr<RunSession>> runs;
        bool dispatching{};

        Impl(lux::scene::SceneRuntime& host, process::ExecutionRuntime& workers, std::size_t limit)
            : runtime(host), execution(workers), tasks(std::make_shared<process::TaskScope>(workers)), capacity(limit)
        {}
        RunResult<void> access() const noexcept
        {
            if (owner != std::this_thread::get_id())
                return rejected(ERunError::WRONG_THREAD);
            if (dispatching)
                return rejected(ERunError::BUSY);
            return {};
        }
        RunResult<RunSession*> find(RunId id) const noexcept
        {
            auto allowed = access();
            if (!allowed)
                return lux::cxx::unexpected(allowed.error());
            if (!id.valid() || id.domain != domain)
                return rejected(ERunError::INVALID_ID);
            const auto* record = runs.find({id.slot, id.generation});
            if (!record || !*record)
                return rejected(ERunError::INVALID_ID);
            return record->get();
        }
        RunResult<void> beginPause(RunSession& run)
        {
            for (const auto ticket : run.steps)
            {
                const auto status = runtime.stepStatus(ticket);
                if (!status)
                    return rejected(status.error());
                const auto state = status->state;
                if (state == lux::scene::ESceneStepState::QUEUED || state == lux::scene::ESceneStepState::EXECUTING)
                    return {};
            }
            if (!runtime.borrowInstance(run.info.instance))
                return {}; // A prior step's stable/publication work still owns the instance.
            if (run.debug)
                return {};
            auto history = editing::EditHistory::create({historyLimits, {}});
            if (!history)
                return rejected(history.error());
            run.debug = std::make_unique<RunDebugEdits>(
                std::move(*history),
                runtime,
                run.info.instance,
                run.environment.components
            );
            run.info.state = ERunState::PAUSED;
            run.info.pause_pending = false;
            return {};
        }
        RunResult<StopTicket> stop(RunSession& run)
        {
            if (run.info.state == ERunState::STOPPING || run.info.state == ERunState::STOPPED ||
                run.info.state == ERunState::FAILED)
                return StopTicket{run.info.id, run.retirement};
            auto finished = run.finishEditing();
            if (!finished)
                return lux::cxx::unexpected(finished.error());
            if (run.debug)
            {
                auto closed = editing::EditExecutor{}.close(*run.debug->history);
                if (!closed)
                    return rejected(closed.error());
                run.debug->editing.close();
            }
            run.info.state = ERunState::STOPPING;
            run.info.pause_pending = false;
            run.connections.clear();
            run.info.retired_updates += run.info.pending_updates;
            run.info.pending_updates = 0;
            run.retirement = run.instance.retire();
            return StopTicket{run.info.id, run.retirement};
        }
        RunResult<void> update()
        {
            const auto allowed = access();
            if (!allowed)
                return allowed;
            DispatchScope dispatch(dispatching);
            for (auto& owned : runs)
            {
                auto& run = *owned;
                if (run.info.state == ERunState::STOPPING)
                {
                    if (run.retirement.complete())
                    {
                        run.package = {};
                        run.snapshot.reset();
                        run.environment.assets = {};
                        run.info.retained_resources = 0;
                        run.info.state = run.info.result ? ERunState::STOPPED : ERunState::FAILED;
                    }
                    continue;
                }
                if (run.info.state == ERunState::STOPPED || run.info.state == ERunState::FAILED)
                    continue;
                auto registry = std::as_const(runtime).borrowInstance(run.info.instance);
                if (!registry)
                    return rejected(registry.error());
                run.info.progress =
                    registry->get().ctx().get<std::reference_wrapper<const lux::scene::SceneDriveSnapshot>>().get();
                run.observeRender(registry->get());
                if (!run.info.progress.result)
                {
                    run.info.result =
                        rejected(lux::scene::SceneRuntimeFailure{run.info.instance, run.info.progress.result.error()});
                    auto stopped = stop(run);
                    if (!stopped)
                        return lux::cxx::unexpected(stopped.error());
                    continue;
                }
                if (run.info.pause_pending)
                {
                    auto paused = beginPause(run);
                    if (!paused)
                        return paused;
                }
            }
            return {};
        }
    };

    RunStore::RunStore(lux::scene::SceneRuntime& runtime, process::ExecutionRuntime& execution, std::size_t capacity)
        : impl_(std::make_unique<Impl>(runtime, execution, capacity))
    {}
    RunStore::~RunStore() = default;
    RunResult<RunInfo> RunStore::info(RunId id) const
    {
        auto run = impl_->find(id);
        if (!run)
            return lux::cxx::unexpected(run.error());
        return (*run)->info;
    }
    RunResult<void> RunStore::pause(RunId id) noexcept
    {
        auto found = impl_->find(id);
        if (!found)
            return lux::cxx::unexpected(found.error());
        auto& run = **found;
        if (run.info.state != ERunState::RUNNING)
            return rejected(ERunError::BUSY);
        auto paused = impl_->runtime.pauseSimulation(run.info.instance);
        if (!paused)
            return rejected(paused.error());
        run.info.pause_pending = true;
        return {};
    }
    RunResult<void> RunStore::resume(RunId id)
    {
        auto found = impl_->find(id);
        if (!found)
            return lux::cxx::unexpected(found.error());
        auto& run = **found;
        if (run.info.state != ERunState::PAUSED)
            return rejected(ERunError::BUSY);
        DispatchScope dispatch(impl_->dispatching);
        const auto finished = run.finishEditing();
        if (!finished)
            return finished;
        auto resumed = impl_->runtime.resumeSimulation(run.info.instance);
        if (!resumed)
            return rejected(resumed.error());
        auto closed = run.closeDebug();
        if (!closed)
            return closed;
        run.info.state = ERunState::RUNNING;
        run.info.pause_pending = false;
        return {};
    }
    RunResult<StepTicket> RunStore::step(RunId id)
    {
        auto found = impl_->find(id);
        if (!found)
            return lux::cxx::unexpected(found.error());
        auto& run = **found;
        if (run.info.state != ERunState::PAUSED)
            return rejected(ERunError::BUSY);
        DispatchScope dispatch(impl_->dispatching);
        const auto finished = run.finishEditing();
        if (!finished)
            return lux::cxx::unexpected(finished.error());
        auto ticket = impl_->runtime.requestStep(run.info.instance);
        if (!ticket)
            return rejected(ticket.error());
        run.steps.push_back(*ticket);
        const auto closed = run.closeDebug();
        if (!closed)
            return lux::cxx::unexpected(closed.error());
        run.info.pause_pending = true;
        return StepTicket{id, *ticket};
    }
    RunResult<lux::scene::SceneStepStatus> RunStore::stepStatus(StepTicket ticket) const noexcept
    {
        auto found = impl_->find(ticket.run);
        if (!found)
            return lux::cxx::unexpected(found.error());
        if ((*found)->info.instance != ticket.step.scene)
            return rejected(ERunError::INVALID_ID);
        DispatchScope reading(impl_->dispatching);
        auto status = impl_->runtime.stepStatus(ticket.step, (*found)->retirement);
        if (!status)
            return rejected(status.error());
        return *status;
    }
    RunResult<void> RunStore::acknowledgeStep(StepTicket ticket) noexcept
    {
        auto status = stepStatus(ticket);
        if (!status)
            return lux::cxx::unexpected(status.error());
        auto run = impl_->find(ticket.run);
        DispatchScope dispatch(impl_->dispatching);
        auto acknowledged = impl_->runtime.acknowledgeStep(ticket.step, (*run)->retirement);
        if (!acknowledged)
            return rejected(acknowledged.error());
        std::erase((*run)->steps, ticket.step);
        return {};
    }
    RunResult<StopTicket> RunStore::stop(RunId id)
    {
        auto run = impl_->find(id);
        if (!run)
            return lux::cxx::unexpected(run.error());
        DispatchScope dispatch(impl_->dispatching);
        return impl_->stop(**run);
    }
    RunResult<void> RunStore::acknowledgeStop(RunId id)
    {
        auto run = impl_->find(id);
        if (!run)
            return lux::cxx::unexpected(run.error());
        const auto state = (*run)->info.state;
        if (state != ERunState::STOPPED && state != ERunState::FAILED)
            return rejected(ERunError::BUSY);
        DispatchScope dispatch(impl_->dispatching);
        // Final Run acknowledgement also clears results held by external StopTicket copies.
        for (const auto ticket : (*run)->steps)
        {
            const auto acknowledged = impl_->runtime.acknowledgeStep(ticket, (*run)->retirement);
            if (!acknowledged)
                return rejected(acknowledged.error());
        }
        impl_->runs.erase({id.slot, id.generation});
        return {};
    }
    RunResult<void> RunStore::update()
    {
        return impl_->update();
    }
    RunResult<std::reference_wrapper<SceneEditing>> RunStore::debugEditing(RunId id) noexcept
    {
        auto run = impl_->find(id);
        if (!run)
            return lux::cxx::unexpected(run.error());
        if (!(*run)->debug)
            return rejected(ERunError::BUSY);
        return std::ref((*run)->debug->editing);
    }
    RunResult<std::reference_wrapper<editing::EditHistory>> RunStore::debugHistory(RunId id) noexcept
    {
        auto run = impl_->find(id);
        if (!run)
            return lux::cxx::unexpected(run.error());
        if (!(*run)->debug)
            return rejected(ERunError::BUSY);
        return std::ref(*(*run)->debug->history);
    }
    RunResult<void> RunStore::finishEditing(RunId id)
    {
        auto run = impl_->find(id);
        if (!run)
            return lux::cxx::unexpected(run.error());
        DispatchScope dispatch(impl_->dispatching);
        return (*run)->finishEditing();
    }

    RunResult<void> RunStore::withInspection(RunningObjectRef target, Inspect inspect)
    {
        auto found = impl_->find(target.run);
        if (!found)
            return cxx::unexpected(found.error());
        auto& run = **found;
        if (run.info.instance != target.instance)
            return rejected(ERunError::INVALID_ID);
        if (!run.instance)
            return rejected(ERunError::STOPPED);
        auto registry = std::as_const(impl_->runtime).borrowInstance(target.instance);
        if (!registry)
            return rejected(registry.error());
        if (!registry->get().valid(target.entity))
            return rejected(ERunError::INVALID_ID);
        DispatchScope dispatch(impl_->dispatching);
        std::optional<editing::HistorySnapshot> history;
        if (run.debug && run.info.state == ERunState::PAUSED && !run.info.pause_pending)
        {
            auto view = run.debug->history->view();
            if (!view)
                return rejected(view.error());
            history = view->snapshot;
        }
        return inspect(registry->get(), history);
    }
    RunResult<void> RunStore::withEditing(
        RunningObjectRef target,
        editing::StateId expected,
        editing::Revision revision,
        Edit edit
    )
    {
        auto found = impl_->find(target.run);
        if (!found)
            return cxx::unexpected(found.error());
        auto& run = **found;
        if (run.info.instance != target.instance)
            return rejected(ERunError::INVALID_ID);
        if (!run.debug || run.info.state != ERunState::PAUSED || run.info.pause_pending)
            return rejected(ERunError::BUSY);
        DispatchScope dispatch(impl_->dispatching);
        auto current = run.debug->history->view();
        if (!current)
            return rejected(current.error());
        if (current->snapshot.current != expected || current->snapshot.revision != revision)
            return rejected(editing::makeEditFailure(editing::EEditError::STALE_BASE));
        return edit(run.debug->editing);
    }

    RunInspectAccess RunStore::inspect() const noexcept
    {
        return RunInspectAccess(
            *this,
            +[](const RunStore& store, RunningObjectRef ref) noexcept {
                auto run = store.impl_->find(ref.run);
                if (!run || (*run)->info.instance != ref.instance)
                    return false;
                auto registry = store.inspect().borrow(ref.run);
                return registry && registry->get().valid(ref.entity);
            },
            +[](const RunStore& store, RunId id) noexcept -> RunInspectAccess::BorrowResult {
                auto run = store.impl_->find(id);
                if (!run)
                    return lux::cxx::unexpected(run.error());
                if (!(*run)->instance)
                    return rejected(ERunError::STOPPED);
                auto borrowed = std::as_const(store.impl_->runtime).borrowInstance((*run)->info.instance);
                if (!borrowed)
                    return rejected(borrowed.error());
                return *borrowed;
            },
            +[](const RunStore& store, RunId id) { return store.info(id); },
            +[](const RunStore& store, RunId id, simulation::ecs::Entity entity
             ) noexcept -> RunResult<RunningObjectRef> {
                auto run = store.impl_->find(id);
                if (!run)
                    return lux::cxx::unexpected(run.error());
                RunningObjectRef ref{id, (*run)->info.instance, entity};
                if (!store.inspect().contains(ref))
                    return rejected(ERunError::INVALID_ID);
                return ref;
            }
        );
    }

    struct StartRunOperation::Impl final
    {
        StartRunId id;
        RunEnvironment environment;
        RunProvenance provenance;
        std::shared_ptr<const SceneSnapshot> snapshot;
        std::stop_source stop;
        std::optional<RunResult<lux::scene::ScenePackage>> completed;
        process::Task task;
        bool adopted{};
    };
    StartRunOperation::StartRunOperation(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
    StartRunOperation::~StartRunOperation()
    {
        cancel();
    }
    void StartRunOperation::cancel() noexcept
    {
        impl_->stop.request_stop();
        impl_->task.requestStop();
    }
    bool StartRunOperation::ready() const noexcept
    {
        return impl_->completed.has_value();
    }
    StartRunId StartRunOperation::id() const noexcept
    {
        return impl_->id;
    }

    RunResult<std::unique_ptr<StartRunOperation>> RunStore::launch(std::shared_ptr<StartRunOperation::Impl> state)
    {
        auto allowed = impl_->access();
        if (!allowed)
            return lux::cxx::unexpected(allowed.error());
        const auto step = state->provenance.configuration.fixed_step;
        if (step.count() <= 0 || step > std::chrono::seconds(1) || !state->environment.simulation_systems)
            return rejected(ERunError::INVALID_CONFIGURATION);
        if (impl_->runs.size() == impl_->capacity)
            return rejected(ERunError::CAPACITY);
        if (impl_->next_start == UINT64_MAX)
            return rejected(ERunError::CAPACITY);
        state->id = {impl_->domain, impl_->next_start++};
        DispatchScope dispatch(impl_->dispatching);
        auto& execution = impl_->execution;
        auto submitted = execution.submit(
            {"Prepare scene run", "scene"},
            [state, cpu = execution.cpu()](process::TaskReporter reporter) noexcept {
                reporter.setPhase("Assemble frozen scene");
                return stdexec::then(stdexec::schedule(cpu), [state]() -> RunResult<lux::scene::ScenePackage> {
                    auto package = buildSceneSnapshotPackage(*state->snapshot, state->stop.get_token());
                    if (!package)
                        return rejected(package.error());
                    return std::move(*package);
                });
            },
            [state](process::TTaskResult<lux::scene::ScenePackage, RunFailure>&& result) noexcept {
                // Accepted completion is a leaf fact, independent of RunStore dispatch admission.
                // Do not call owner/UI, reset an outer guard, or retry the encoder.
                if (result)
                    state->completed.emplace(std::move(*result));
                else if (auto* error = result.error().domainFailure())
                    state->completed.emplace(lux::cxx::unexpected(std::move(*error)));
                else if (const auto* error = result.error().executionFailure())
                    state->completed.emplace(rejected(*error));
                else
                    state->completed.emplace(rejected(ERunError::CANCELLED));
                state->task = {};
            }
        );
        if (!submitted)
            return rejected(submitted.error());
        state->task = std::move(*submitted);
        return std::unique_ptr<StartRunOperation>(new StartRunOperation(std::move(state)));
    }
    RunResult<std::unique_ptr<StartRunOperation>> RunStore::prepare(
        SceneSession& author,
        RunEnvironment environment,
        RunConfiguration configuration
    )
    {
        auto frozen = author.capture();
        if (!frozen)
            return rejected(frozen.error());
        return prepare(std::move(*frozen), std::move(environment), configuration);
    }
    RunResult<std::unique_ptr<StartRunOperation>> RunStore::prepare(
        SceneSnapshot snapshot,
        RunEnvironment environment,
        RunConfiguration configuration
    )
    {
        auto state = std::make_shared<StartRunOperation::Impl>();
        state->environment = std::move(environment);
        state->provenance = {snapshot.content(), configuration};
        state->snapshot = std::make_shared<const SceneSnapshot>(std::move(snapshot));
        return launch(std::move(state));
    }
    RunResult<RunId> RunStore::adopt(StartRunOperation& operation)
    {
        auto& store = *impl_;
        auto allowed = store.access();
        if (!allowed)
            return lux::cxx::unexpected(allowed.error());
        auto& prepared = *operation.impl_;
        if (prepared.id.domain != store.domain)
            return rejected(ERunError::INVALID_ID);
        if (prepared.adopted)
            return rejected(ERunError::INVALID_ID);
        if (!prepared.completed)
            return rejected(ERunError::NOT_READY);
        if (prepared.stop.stop_requested())
            return rejected(ERunError::CANCELLED);
        if (!*prepared.completed)
            return lux::cxx::unexpected(prepared.completed->error());
        if (store.runs.size() == store.capacity)
            return rejected(ERunError::CAPACITY);
        DispatchScope dispatch(store.dispatching);
        prepared.adopted = true; // Consuming input is one attempt, including a failed factory.
        auto run = std::make_unique<RunSession>();
        run->steps.reserve(32);
        run->environment = std::move(prepared.environment);
        run->snapshot = std::move(prepared.snapshot);
        run->package = std::move(**prepared.completed);
        auto& source = run->package;
        auto& environment = run->environment;
        auto storage = process::world_loading::makeWorldMemoryStorageSource(
            std::shared_ptr<const world::WorldDescription>(source.world, &source.world->data()),
            source.volumes
        );
        if (!storage)
            return rejected(ERunError::INVALID_CONFIGURATION);
        lux::scene::WorldLoadingServices loading{std::move(*storage), *store.tasks};
        auto bindings = lux::scene::RenderFeatureSceneBindings(environment.render_bindings);
        auto timer = store.execution.timer();
        std::vector<lux::scene::SceneCapabilityProvider> providers;
        providers.push_back(lux::scene::makeSceneCapabilityProvider<lux::scene::WorldLoadingServices>(
            "world-storage",
            "lux.world.loading",
            loading
        ));
        if (environment.renderer && environment.resources)
        {
            providers.push_back(lux::scene::makeSceneCapabilityProvider<render::RenderRuntime>(
                "main-window",
                "lux.render.runtime",
                *environment.renderer
            ));
            providers.push_back(lux::scene::makeSceneCapabilityProvider<lux::scene::RenderResources>(
                "resources",
                "lux.render.resources",
                *environment.resources
            ));
            providers.push_back(lux::scene::makeSceneCapabilityProvider<lux::scene::RenderFeatureSceneBindings>(
                "render-bindings",
                "lux.render.scene_bindings",
                bindings
            ));
            providers.push_back(lux::scene::makeSceneCapabilityProvider<lux::scene::RenderAssetInput>(
                "assets",
                "lux.render.assets",
                environment.assets
            ));
        }
        if (environment.scripts)
        {
            providers.push_back(lux::scene::makeSceneCapabilityProvider<lux::scene::ScriptRuntimeHost>(
                "script-runtime", "lux.script.runtime.host", *environment.scripts
            ));
            providers.push_back(lux::scene::makeSceneCapabilityProvider<process::TimerClient>(
                "timer", "lux.process.timer", timer
            ));
        }
        const auto clock = lux::scene::FixedStepClock::create(prepared.provenance.configuration.fixed_step);
        if (!clock)
            return rejected(ERunError::INVALID_CONFIGURATION);
        auto instance =
            store.runtime.builder()
                .setDescription(std::shared_ptr<const lux::scene::SceneDescription>(source.scene, &source.scene->data())
                )
                .setWorld(std::shared_ptr<const world::WorldDescription>(source.world, &source.world->data()))
                .setSimulation(std::shared_ptr<const simulation::SimulationDescription>(
                    source.simulation,
                    &source.simulation->data()
                ))
                .setRegistrations(environment.components, *environment.simulation_systems, environment.scene_systems)
                .setProviders(providers)
                .setClock(*clock)
                .build();
        if (!instance)
            return rejected(instance.error());
        store.runtime.borrowInstance(instance->id())
            ->get()
            .ctx()
            .emplace<std::shared_ptr<process::TaskScope>>(store.tasks);
        run->instance = std::move(*instance);
        run->info.instance = run->instance.id();
        run->info.provenance = prepared.provenance;
        run->observe(store.runtime.borrowInstance(run->info.instance)->get());
        const auto slot = store.runs.emplace(std::move(run));
        const RunId id{store.domain, slot.index, slot.gen};
        (*store.runs.find(slot))->info.id = id;
        prepared.task = {};
        return id;
    }
}

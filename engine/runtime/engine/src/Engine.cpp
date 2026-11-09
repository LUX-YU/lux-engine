#include <lux/engine/Engine.hpp>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/SceneBuilder.hpp>

#include <exec/create.hpp>

#include <lux/cxx/container/SlotMap.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/detail/SceneSession.hpp>
#include <lux/engine/scene/SceneSystem.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationSystem.hpp>
#include <lux/engine/simulation/task/SystemTaskResources.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <algorithm>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace lux::engine
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        constexpr auto IdentityExhausted = error::errorId("lux.engine.scene.identity_exhausted");
        constexpr auto InvalidScene = error::errorId("lux.engine.scene.invalid_id");
        constexpr auto TimeOverflow = error::errorId("lux.engine.scene.time_overflow");
        constexpr auto GraphBuildFailed = error::errorId("lux.engine.scene.graph_build");
        constexpr auto GraphExecutionFailed = error::errorId("lux.engine.scene.graph_execution");

        Engine::Result invalidState() noexcept
        {
            return cxx::unexpected(execution::toError(execution::EExecutionError::INVALID_STATE));
        }

        Engine::Result buildGraphs(SceneSession& session, scene::SceneInstance& scene) noexcept
        {
            const auto groups = scene.simulation().groups();
            if (groups.size() > UINT32_MAX)
            {
                return cxx::unexpected(error::Error{IdentityExhausted});
            }
            session.groups.resize(groups.size());
            for (std::size_t index = 0; index < groups.size(); ++index)
            {
                const auto& group = groups[index];
                if (const auto step = group.fixedStep(); step && *step <= Engine::Duration::zero())
                {
                    return cxx::unexpected(execution::toError(execution::EExecutionError::INVALID_ARGUMENT));
                }
                task::TaskGraphBuilder builder;
                for (const auto& system : group.systems())
                {
                    auto added = builder.add(
                        simulation::systemTaskResources(system->access()),
                        task::on(system->taskAffinity()),
                        [system = system.get(), time = &session.groups[index].time]() noexcept
                        {
                            if (system->isEnabled())
                            {
                                system->execute(*time);
                            }
                        }
                    );
                    if (!added)
                    {
                        const auto& failure = added.error();
                        return cxx::unexpected(error::Error{
                            GraphBuildFailed,
                            {static_cast<std::uint64_t>(failure.code), failure.task.index, failure.related.index}
                        });
                    }
                }
                auto graph = std::move(builder).build();
                if (!graph)
                {
                    const auto& failure = graph.error();
                    return cxx::unexpected(error::Error{
                        GraphBuildFailed,
                        {static_cast<std::uint64_t>(failure.code), failure.task.index, failure.related.index}
                    });
                }
                session.groups[index].graph = std::move(*graph);
            }
            return {};
        }

        Engine::Result executeGraph(task::TaskExecutor& executor, const task::TaskGraph& graph) noexcept
        {
            if (graph.taskCount() == 0)
            {
                return {};
            }
            // The local run_loop drives CALLER_THREAD nodes, never unrelated Engine/Object callbacks.
            auto completed = stdexec::sync_wait(
                executor.schedule(graph) | stdexec::then([]() noexcept { return Engine::Result{}; }) |
                stdexec::upon_error(
                    [](task::ETaskExecutorError failure) noexcept -> Engine::Result
                    {
                        return cxx::unexpected(error::Error{GraphExecutionFailed, {static_cast<std::uint64_t>(failure)}}
                        );
                    }
                ) |
                stdexec::upon_stopped(
                    []() noexcept -> Engine::Result
                    { return cxx::unexpected(execution::toError(execution::EExecutionError::STOPPING)); }
                )
            );
            return std::move(std::get<0>(*completed));
        }
    } // namespace

    struct Engine::Impl final
    {
        struct ScheduledTick final
        {
            TimePoint deadline;
            scene::SceneInstanceId scene;
        };

        using VStageCallback = std::variant<EngineStageCallback, EngineStageAsyncCallback>;

        explicit Impl(EngineContext& context) noexcept : context(context), executor(context.execution().cpuScheduler())
        {
        }

        static bool later(const ScheduledTick& left, const ScheduledTick& right) noexcept
        {
            return std::tie(left.deadline, left.scene) > std::tie(right.deadline, right.scene);
        }

        TimePoint nextDeadline() const noexcept
        {
            return fixed_ticks.empty() ? TimePoint::max() : fixed_ticks.front().deadline;
        }

        bool hasContinuousWork() const noexcept
        {
            for (const auto& session : scenes)
            {
                if (session->tasks.stopToken().stop_requested() || !session->initialization ||
                    !*session->initialization)
                {
                    continue;
                }
                for (const auto& group : session->scene->simulation().groups())
                {
                    if (session->real_anchor && !group.fixedStep() && !group.systems().empty())
                    {
                        return true;
                    }
                }
                for (const auto& system : session->scene->systems())
                {
                    if (system->isEnabled())
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        // Called only outside graph execution and System/Collector traversal. A Main continuation may
        // adopt ECS data here; observers must queue roots rather than recursively drive this dispatcher.
        Result dispatchEvents(const EventPump& pump) noexcept
        {
            if (pump)
            {
                pump();
            }
            const auto dispatched = context.execution().dispatch();
            if (!dispatched)
            {
                return cxx::unexpected(execution::toError(dispatched.error()));
            }
            return stop_requested ? Result{} : applyCommands();
        }

        void waitForWork(std::uint64_t epoch, TimePoint deadline, const EventWait& wait) noexcept
        {
            auto& execution = context.execution();
            if (execution.hasReadyWork() || execution.wakeEpoch() != epoch)
            {
                return;
            }
            if (wait)
            {
                wait(deadline);
            }
            else
            {
                execution.waitForWork(epoch, deadline);
            }
        }

        Result tick(SceneSession::GroupExecution& group, Duration delta) noexcept
        {
            const bool invalid_delta = delta < Duration::zero();
            const bool elapsed_overflow = !invalid_delta && delta > Duration::max() - group.time.elapsed;
            const bool step_overflow = group.time.step_index == UINT64_MAX;
            if (invalid_delta || elapsed_overflow || step_overflow)
            {
                return cxx::unexpected(error::Error{TimeOverflow});
            }
            const auto previous = group.time;
            group.time = {previous.elapsed + delta, delta, previous.step_index + 1};
            auto executed = executeGraph(executor, group.graph);
            if (!executed)
            {
                group.time = previous; // Executor rejection precedes execution of any node.
            }
            return executed;
        }

        static bool ready(const SceneSession& session) noexcept
        {
            return !session.tasks.stopToken().stop_requested() && session.initialization && *session.initialization;
        }

        static Duration due(const SceneSession& session, std::uint32_t index) noexcept
        {
            return session.groups[index].time.elapsed + *session.scene->simulation().groups()[index].fixedStep();
        }

        static auto groupOrder(const SceneSession& session) noexcept
        {
            return [&session](std::uint32_t left, std::uint32_t right) noexcept
            {
                const auto left_due = due(session, left);
                const auto right_due = due(session, right);
                return std::tie(left_due, left) > std::tie(right_due, right);
            };
        }

        static cxx::expected<Duration, error::Error> simulationTime(const SceneSession& session, TimePoint now) noexcept
        {
            if (!session.real_anchor || now <= *session.real_anchor)
            {
                return session.simulation_anchor;
            }
            const auto elapsed = now - *session.real_anchor;
            const auto remaining = Duration::max() - session.simulation_anchor;
            if (elapsed > std::chrono::duration_cast<Clock::duration>(remaining))
            {
                return cxx::unexpected(error::Error{TimeOverflow});
            }
            return session.simulation_anchor + std::chrono::duration_cast<Duration>(elapsed);
        }

        Result scheduleFixed(scene::SceneInstanceId id, const SceneSession& session) noexcept
        {
            if (!session.real_anchor || session.fixed_groups.empty())
            {
                return {};
            }
            const auto offset = std::chrono::ceil<Clock::duration>(
                due(session, session.fixed_groups.front()) - session.simulation_anchor
            );
            const bool overflows = offset > Clock::duration::zero() &&
                *session.real_anchor > TimePoint::max() - offset;
            const bool underflows = offset < Clock::duration::zero() &&
                *session.real_anchor < TimePoint::min() - offset;
            if (overflows || underflows)
            {
                return cxx::unexpected(error::Error{TimeOverflow});
            }
            fixed_ticks.push_back({*session.real_anchor + offset, id});
            std::push_heap(fixed_ticks.begin(), fixed_ticks.end(), later);
            return {};
        }

        static void preserveProgress(SceneSession& session) noexcept
        {
            session.real_anchor.reset();
            for (const auto& group : session.groups)
            {
                session.simulation_anchor = std::max(session.simulation_anchor, group.time.elapsed);
            }
        }

        Result executeSystems(SceneSession& session, scene::ESceneSystemStage stage) noexcept
        {
            for (auto* system : session.stage_systems[static_cast<std::size_t>(stage)])
            {
                if (!system->isEnabled())
                {
                    continue;
                }
                if (auto executed = system->execute(); !executed)
                {
                    return executed;
                }
                if (stage == scene::ESceneSystemStage::AFTER_SIMULATION)
                {
                    if (auto applied = session.scene->applyCommands(session.commands); !applied)
                    {
                        return applied;
                    }
                }
            }
            return stage == scene::ESceneSystemStage::BEFORE_SIMULATION
                ? session.scene->applyCommands(session.commands) : Result{};
        }

        Result advanceFixedGroup(SceneSession& session) noexcept
        {
            auto before = executeSystems(session, scene::ESceneSystemStage::BEFORE_SIMULATION);
            if (!before || stop_requested)
            {
                return before;
            }
            std::pop_heap(session.fixed_groups.begin(), session.fixed_groups.end(), groupOrder(session));
            const auto index = session.fixed_groups.back();
            session.fixed_groups.pop_back();
            const auto step = *session.scene->simulation().groups()[index].fixedStep();
            auto executed = tick(session.groups[index], step);
            // Exhausted logical time has no next deadline; never overflow while comparing heap keys.
            if (session.groups[index].time.elapsed <= Duration::max() - step)
            {
                session.fixed_groups.push_back(index);
                std::push_heap(session.fixed_groups.begin(), session.fixed_groups.end(), groupOrder(session));
            }
            return executed ? session.scene->applyCommands(session.commands) : executed;
        }

        Result advanceFixed(TimePoint now) noexcept
        {
            while (!stop_requested && !fixed_ticks.empty() && fixed_ticks.front().deadline <= now)
            {
                std::pop_heap(fixed_ticks.begin(), fixed_ticks.end(), later);
                const auto scheduled = fixed_ticks.back();
                fixed_ticks.pop_back();
                auto& session = **scenes.find({scheduled.scene.slot, scheduled.scene.generation});
                auto executed = advanceFixedGroup(session);
                if (executed)
                {
                    executed = scheduleFixed(scheduled.scene, session);
                }
                if (!executed)
                {
                    preserveProgress(session);
                    return executed;
                }
            }
            return {};
        }

        Result advanceVariable(TimePoint now) noexcept
        {
            for (auto& session : scenes)
            {
                if (!ready(*session))
                {
                    continue;
                }
                auto before = executeSystems(*session, scene::ESceneSystemStage::BEFORE_SIMULATION);
                if (!before)
                {
                    preserveProgress(*session);
                    return before;
                }
                if (!session->real_anchor || stop_requested)
                {
                    continue;
                }
                auto target = simulationTime(*session, now);
                if (!target)
                {
                    preserveProgress(*session);
                    return cxx::unexpected(target.error());
                }
                const auto groups = session->scene->simulation().groups();
                for (std::size_t index = 0; index < groups.size(); ++index)
                {
                    auto& group = session->groups[index];
                    if (groups[index].fixedStep() || *target <= group.time.elapsed)
                    {
                        continue;
                    }
                    auto executed = tick(group, *target - group.time.elapsed);
                    if (executed)
                    {
                        executed = session->scene->applyCommands(session->commands);
                    }
                    if (!executed)
                    {
                        preserveProgress(*session);
                        return executed;
                    }
                }
            }
            return {};
        }

        Result applyCommands() noexcept
        {
            for (auto& session : scenes)
            {
                if (ready(*session))
                {
                    if (auto applied = session->scene->applyCommands(session->commands); !applied)
                    {
                        return applied;
                    }
                }
            }
            return {};
        }

        Result executeSystems() noexcept
        {
            for (auto& session : scenes)
            {
                if (ready(*session))
                {
                    auto executed = executeSystems(*session, scene::ESceneSystemStage::AFTER_SIMULATION);
                    if (!executed)
                    {
                        return executed;
                    }
                }
            }
            return {};
        }

        void completeInitialization(SceneSession& session, scene::SceneInstanceId id, Result result) noexcept
        {
            if (result && session.tasks.stopToken().stop_requested())
            {
                result = cxx::unexpected(execution::toError(execution::EExecutionError::STOPPING));
            }
            if (result && session.real_anchor)
            {
                session.real_anchor = Clock::now();
                result = scheduleFixed(id, session);
            }
            session.initialization.emplace(result);
            auto complete = std::move(session.initialization_completion);
            if (complete)
            {
                complete(std::move(result)); // Terminal delivery is last; it may start removal.
            }
        }

        template <class Sender>
        Result await(Sender sender, const EventPump& pump, const EventWait& wait, bool advance_fixed) noexcept
        {
            auto& execution = context.execution();
            std::optional<Result> completed;
            auto normalized = stdexec::starts_on(execution.mainScheduler(), std::move(sender)) |
                              stdexec::upon_error([](std::exception_ptr) noexcept -> Result { std::terminate(); }) |
                              stdexec::upon_stopped(
                                  []() noexcept -> Result
                                  { return cxx::unexpected(execution::toError(execution::EExecutionError::STOPPING)); }
                              );
            auto accepted = execution.spawn(stdexec::write_env(
                stdexec::then(
                    std::move(normalized),
                    [&completed](Result result) noexcept { completed.emplace(std::move(result)); }
                ),
                stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
            ));
            if (!accepted)
            {
                return cxx::unexpected(execution::toError(accepted.error()));
            }
            Result result;
            while (!completed)
            {
                const auto epoch = execution.wakeEpoch();
                if (auto dispatched = dispatchEvents(pump); !dispatched)
                {
                    // A command barrier can fail after applying a prefix. Stop normal rounds, but retain
                    // this outer driver until the already accepted stage continuation has completed.
                    if (result)
                    {
                        result = std::move(dispatched);
                    }
                    stop_requested = true;
                }
                if (advance_fixed && !stop_requested)
                {
                    if (auto advanced = advanceFixed(Clock::now()); !advanced)
                    {
                        result = std::move(advanced);
                        stop_requested = true;
                    }
                }
                if (!completed)
                {
                    waitForWork(epoch, stop_requested || !advance_fixed ? TimePoint::max() : nextDeadline(), wait);
                }
            }
            return result ? std::move(*completed) : std::move(result);
        }

        Result runStage(EEngineStage stage, const EventPump& pump, const EventWait& wait) noexcept
        {
            const auto found = stage_callbacks.find(stage);
            if (found == stage_callbacks.end())
            {
                return {};
            }
            Result result;
            for (auto& callback : found->second)
            {
                auto invoked = std::holds_alternative<EngineStageCallback>(callback)
                                   ? std::get<EngineStageCallback>(callback)()
                                   : await(std::get<EngineStageAsyncCallback>(callback)(), pump, wait, true);
                if (!invoked)
                {
                    if (result)
                    {
                        result = std::move(invoked);
                    }
                    if (stage != EEngineStage::UPDATE_ABORTED)
                    {
                        break;
                    }
                }
                if (stage == EEngineStage::BEGIN_UPDATE && stop_requested)
                {
                    break;
                }
            }
            return result;
        }

        // The queue owns tasks and terminal delivery, never the objects they borrow.
        // The front entry remains present through resource retirement. Completion can enqueue another root.
        void startRetirement() noexcept
        {
            auto& execution = context.execution();
            auto operation = stdexec::starts_on(execution.mainScheduler(), std::move(retirements.front().first)) |
                             stdexec::upon_error([](std::exception_ptr) noexcept -> Result { std::terminate(); }) |
                             stdexec::upon_stopped(
                                 []() noexcept -> Result
                                 { return cxx::unexpected(execution::toError(execution::EExecutionError::STOPPING)); }
                             );
            auto started = execution.spawn(stdexec::write_env(
                stdexec::then(
                    std::move(operation),
                    [this](Result result) noexcept
                    {
                        auto complete = std::move(retirements.front().second);
                        retirements.pop_front();
                        if (!retirements.empty())
                        {
                            startRetirement(); // starts_on queues the next root; no nested event dispatch.
                        }
                        complete(std::move(result)); // May destroy the Engine when the queue is now empty.
                    }
                ),
                stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
            ));
            if (!started)
            {
                std::terminate(); // EngineContext admission must outlive the Engine's accepted queue.
            }
        }

        auto enqueueRetirement(exec::task<Result> operation) noexcept
        {
            return exec::create<stdexec::set_value_t(Result)>(
                [this, operation = std::move(operation)](auto& state) mutable noexcept
                {
                    const bool was_empty = retirements.empty();
                    retirements.emplace_back(
                        std::move(operation),
                        [&receiver = state.receiver](Result result) noexcept
                        { stdexec::set_value(std::move(receiver), std::move(result)); }
                    );
                    if (was_empty)
                    {
                        startRetirement();
                    }
                }
            );
        }

        exec::task<Result> retireScene(scene::SceneInstanceId id) noexcept
        {
            auto* entry = scenes.find({id.slot, id.generation});
            if (!entry)
            {
                co_return cxx::unexpected(error::Error{InvalidScene, {id.slot, id.generation}});
            }
            auto& session = **entry;
            session.tasks.requestStop();
            co_await stdexec::continues_on(session.tasks.whenSettled(), context.execution().mainScheduler());
            session.groups.clear(); // Graph callbacks must disappear before any System is released.
            session.stage_systems = {};
            session.commands.discardPending();
            auto released = co_await session.scene->releaseResources();
            if (!released)
            {
                co_return released;
            }
            session.commands.discardPending();
            scenes.erase({id.slot, id.generation});
            co_return Result{};
        }

        exec::task<Result> retireScenes() noexcept
        {
            // Earlier queue entries have completed. No new scenes are admitted during final retirement.
            for (auto& session : scenes)
            {
                session->tasks.requestStop();
            }
            for (auto& session : scenes)
            {
                co_await stdexec::continues_on(session->tasks.whenSettled(), context.execution().mainScheduler());
                session->groups.clear();
                session->stage_systems = {};
                session->commands.discardPending();
            }
            for (auto& session : scenes)
            {
                auto released = co_await session->scene->releaseResources();
                if (!released)
                {
                    co_return released;
                }
            }
            for (auto& session : scenes)
            {
                session->commands.discardPending();
            }
            scenes.clear();
            co_return Result{};
        }

        void drainRetirements(const EventPump& pump, const EventWait& wait) noexcept
        {
            auto& execution = context.execution();
            while (!retirements.empty())
            {
                const auto epoch = execution.wakeEpoch();
                if (auto dispatched = dispatchEvents(pump); !dispatched)
                {
                    std::terminate();
                }
                if (!retirements.empty())
                {
                    waitForWork(epoch, TimePoint::max(), wait);
                }
            }
        }

        Result finish(const EventPump& pump, const EventWait& wait) noexcept
        {
            stop_requested = true;
            fixed_ticks.clear();
            drainRetirements(pump, wait);
            for (auto& session : scenes)
            {
                session->tasks.requestStop();
            }
            Result result;
            // No new removals/additions are admitted while this stable directory is being drained.
            for (auto& session : scenes)
            {
                auto settled = await(
                    stdexec::then(
                        stdexec::continues_on(session->tasks.whenSettled(), context.execution().mainScheduler()),
                        []() noexcept { return Result{}; }
                    ),
                    pump,
                    wait,
                    false
                );
                if (!settled && result)
                {
                    result = std::move(settled);
                }
            }
            // Completion of a stopped producer can enqueue its owner's retirement.
            drainRetirements(pump, wait);
            return result;
        }

        EngineContext& context;
        task::TaskExecutor executor;
        cxx::SlotMap<std::unique_ptr<SceneSession>> scenes;
        std::vector<ScheduledTick> fixed_ticks;
        std::map<EEngineStage, std::vector<VStageCallback>> stage_callbacks;
        std::deque<std::pair<exec::task<Result>, std::function<void(Result)>>> retirements;
        bool running = false;
        bool stop_requested = false;
    };

    Engine::Engine(EngineContext& context) noexcept : impl_(std::make_unique<Impl>(context)) {}

    Engine::~Engine() noexcept
    {
        const bool invalid_destruction = !impl_->context.execution().isMainThread() || impl_->running;
        if (invalid_destruction)
        {
            std::terminate(); // The run/removal caller must outlive the operation borrowing this Engine.
        }
        impl_->stop_requested = true;
        impl_->fixed_ticks.clear();
        impl_->drainRetirements({}, {});
        if (impl_->scenes.empty())
        {
            return;
        }
        // This is the outer owner boundary. Context keeps Main delivery and resource services alive.
        // Child destructors never pump events or wait for work borrowing their already-destroyed members.
        auto result = impl_->await(impl_->retireScenes(), {}, {}, false);
        if (!result)
        {
            std::terminate(); // Final shutdown cannot unwind owners whose resource release failed.
        }
    }

    void Engine::addEngineStageCallback(EngineStageCallback callback, EEngineStage stage) noexcept
    {
        impl_->stage_callbacks[stage].emplace_back(std::move(callback));
    }

    void Engine::addEngineStageCallback(EngineStageAsyncCallback callback, EEngineStage stage) noexcept
    {
        if (stage != EEngineStage::BEGIN_UPDATE)
        {
            std::terminate(); // Async admission is permitted only before mutable frame collection starts.
        }
        impl_->stage_callbacks[stage].emplace_back(std::move(callback));
    }

    void Engine::prependEngineStageCallback(EngineStageAsyncCallback callback, EEngineStage stage) noexcept
    {
        auto& callbacks = impl_->stage_callbacks[stage];
        callbacks.insert(callbacks.begin(), std::move(callback));
    }

    Engine::AddSceneResult
    Engine::addScene(std::unique_ptr<scene::SceneInstance>& candidate, bool simulation_paused) noexcept
    {
        const bool is_invalid_state = !impl_->context.execution().isMainThread() || impl_->stop_requested;
        if (is_invalid_state)
        {
            return cxx::unexpected(execution::toError(execution::EExecutionError::INVALID_STATE));
        }
        if (!candidate)
        {
            return cxx::unexpected(execution::toError(execution::EExecutionError::INVALID_ARGUMENT));
        }
        auto session =
            std::unique_ptr<SceneSession>{new SceneSession{execution::TaskScope{impl_->context.execution()}}};
        std::vector<ecs::EcsCommandProducerCapacity> capacities;
        std::vector<std::size_t> producers;
        const auto append_capacity = [&](const auto& system) noexcept -> Result
        {
            const auto capacity = system.commandCapacity();
            if (capacity.max_commands == 0)
            {
                if (capacity.max_payload_bytes != 0)
                {
                    return cxx::unexpected(execution::toError(execution::EExecutionError::INVALID_ARGUMENT));
                }
                producers.push_back(SIZE_MAX);
                return {};
            }
            if (capacities.size() >= UINT32_MAX)
            {
                return cxx::unexpected(error::Error{IdentityExhausted});
            }
            producers.push_back(capacities.size());
            capacities.push_back(capacity);
            return {};
        };
        const auto groups = candidate->simulation().groups();
        if (groups.size() > UINT32_MAX)
        {
            return cxx::unexpected(error::Error{IdentityExhausted});
        }
        for (std::size_t index = 0; index < groups.size(); ++index)
        {
            for (const auto& system : groups[index].systems())
            {
                if (auto appended = append_capacity(*system); !appended)
                {
                    return cxx::unexpected(appended.error());
                }
            }
            if (groups[index].fixedStep())
            {
                session->fixed_groups.push_back(static_cast<std::uint32_t>(index));
            }
        }
        for (const auto& system : candidate->systems())
        {
            const auto stage = static_cast<std::size_t>(system->stage());
            if (stage >= session->stage_systems.size())
            {
                return cxx::unexpected(execution::toError(execution::EExecutionError::INVALID_ARGUMENT));
            }
            session->stage_systems[stage].push_back(system.get());
            if (auto appended = append_capacity(*system); !appended)
            {
                return cxx::unexpected(appended.error());
            }
        }
        auto prepared = session->commands.prepare(capacities);
        if (!prepared)
        {
            const auto& failure = prepared.error();
            return cxx::unexpected(error::Error{
                error::errorId("lux.scene.ecs_commands"),
                {static_cast<std::uint64_t>(failure.code), failure.producer, failure.command}
            });
        }
        if (auto built = buildGraphs(*session, *candidate); !built)
        {
            return cxx::unexpected(built.error());
        }
        if (!simulation_paused)
        {
            session->real_anchor = Clock::now();
        }
        decltype(impl_->scenes)::key_type key;
        try
        {
            key = impl_->scenes.emplace(std::move(session));
        }
        catch (const std::length_error&)
        {
            return cxx::unexpected(error::Error{IdentityExhausted});
        }
        auto& owned = **impl_->scenes.find(key);
        owned.scene = std::move(candidate);
        owned.scene->bindExecution(owned.tasks, owned.commands, producers);
        std::make_heap(owned.fixed_groups.begin(), owned.fixed_groups.end(), Impl::groupOrder(owned));
        const scene::SceneInstanceId id{key.index, key.gen};
        auto accepted = owned.tasks.spawnOnMain(
            stdexec::write_env(
                owned.scene->initialize(owned.commands),
                stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
            ),
            [this, id, &owned](auto outcome) noexcept
            {
                Result initialized;
                if (!outcome)
                {
                    const auto* domain = outcome.error().domainFailure();
                    const auto* execution = outcome.error().executionFailure();
                    initialized = cxx::unexpected(
                        domain ? *domain
                               : execution::toError(execution ? *execution : execution::EExecutionError::STOPPING)
                    );
                }
                impl_->completeInitialization(owned, id, std::move(initialized));
            }
        );
        if (!accepted)
        {
            impl_->completeInitialization(owned, id, cxx::unexpected(execution::toError(accepted.error())));
        }
        return id;
    }

    Engine::Result Engine::setSimulationPaused(scene::SceneInstanceId id, bool paused) noexcept
    {
        const bool is_invalid_state = !impl_->context.execution().isMainThread() || impl_->stop_requested;
        if (is_invalid_state)
        {
            return invalidState();
        }
        auto* entry = impl_->scenes.find({id.slot, id.generation});
        if (!entry)
        {
            return cxx::unexpected(error::Error{InvalidScene, {id.slot, id.generation}});
        }
        auto& session = **entry;
        if (session.tasks.stopToken().stop_requested())
        {
            return invalidState();
        }
        if (paused == !session.real_anchor.has_value())
        {
            return {};
        }
        const auto now = Clock::now();
        if (paused)
        {
            if (session.initialization && *session.initialization)
            {
                auto time = Impl::simulationTime(session, now);
                if (!time)
                {
                    return cxx::unexpected(time.error());
                }
                session.simulation_anchor = *time;
            }
            session.real_anchor.reset();
            std::erase_if(impl_->fixed_ticks, [id](const auto& tick) noexcept { return tick.scene == id; });
            std::make_heap(impl_->fixed_ticks.begin(), impl_->fixed_ticks.end(), Impl::later);
        }
        else
        {
            session.real_anchor = now;
            if (Impl::ready(session))
            {
                if (auto scheduled = impl_->scheduleFixed(id, session); !scheduled)
                {
                    session.real_anchor.reset();
                    return scheduled;
                }
            }
        }
        impl_->context.execution().wake();
        return {};
    }

    Engine::SceneBuilder::SceneBuilder(Engine& engine, const EngineMetaFunctionRegistry& metadata) noexcept
        : context_(engine.impl_->context), metadata_(metadata)
    {
    }

    Engine::Result Engine::advanceSimulation(scene::SceneInstanceId id, Duration delta) noexcept
    {
        const bool is_invalid_state = !impl_->context.execution().isMainThread() || impl_->stop_requested;
        if (is_invalid_state)
        {
            return invalidState();
        }
        auto* entry = impl_->scenes.find({id.slot, id.generation});
        if (!entry)
        {
            return cxx::unexpected(error::Error{InvalidScene, {id.slot, id.generation}});
        }
        auto& session = **entry;
        if (!Impl::ready(session) || session.real_anchor)
        {
            return invalidState();
        }
        if (delta <= Duration::zero())
        {
            return cxx::unexpected(execution::toError(execution::EExecutionError::INVALID_ARGUMENT));
        }
        if (delta > Duration::max() - session.simulation_anchor)
        {
            return cxx::unexpected(error::Error{TimeOverflow});
        }
        const auto target = session.simulation_anchor + delta;
        const auto groups = session.scene->simulation().groups();
        for (std::size_t index = 0; index < groups.size(); ++index)
        {
            const auto& time = session.groups[index].time;
            const auto remaining = target - time.elapsed;
            const auto step = groups[index].fixedStep();
            const auto steps = step ? static_cast<std::uint64_t>(remaining / *step)
                                    : std::uint64_t{remaining > Duration{}};
            if (steps > UINT64_MAX - time.step_index)
            {
                return cxx::unexpected(error::Error{TimeOverflow});
            }
        }
        auto applied = session.scene->applyCommands(session.commands);
        if (!applied)
        {
            return applied;
        }
        while (!impl_->stop_requested && !session.fixed_groups.empty() &&
               Impl::due(session, session.fixed_groups.front()) <= target)
        {
            if (auto executed = impl_->advanceFixedGroup(session); !executed)
            {
                Impl::preserveProgress(session);
                return executed;
            }
        }
        auto before = impl_->executeSystems(session, scene::ESceneSystemStage::BEFORE_SIMULATION);
        if (!before)
        {
            Impl::preserveProgress(session);
            return before;
        }
        for (std::size_t index = 0; index < groups.size() && !impl_->stop_requested; ++index)
        {
            auto& group = session.groups[index];
            if (groups[index].fixedStep() || target <= group.time.elapsed)
            {
                continue;
            }
            auto executed = impl_->tick(group, target - group.time.elapsed);
            if (executed)
            {
                executed = session.scene->applyCommands(session.commands);
            }
            if (!executed)
            {
                Impl::preserveProgress(session);
                return executed;
            }
        }
        if (impl_->stop_requested)
        {
            Impl::preserveProgress(session);
            return cxx::unexpected(execution::toError(execution::EExecutionError::STOPPING));
        }
        session.simulation_anchor = target;
        return {};
    }

    exec::task<Engine::Result> Engine::whenSceneInitialized(scene::SceneInstanceId id) noexcept
    {
        if (!impl_->context.execution().isMainThread())
        {
            co_return cxx::unexpected(execution::toError(execution::EExecutionError::WRONG_THREAD));
        }
        auto* entry = impl_->scenes.find({id.slot, id.generation});
        if (!entry)
        {
            co_return cxx::unexpected(error::Error{InvalidScene, {id.slot, id.generation}});
        }
        auto& session = **entry;
        if (session.initialization)
        {
            co_return *session.initialization;
        }
        if (session.initialization_completion)
        {
            co_return invalidState();
        }
        co_return co_await exec::create<stdexec::set_value_t(Result)>([&session](auto& state) noexcept
        {
            session.initialization_completion = [&receiver = state.receiver](Result result) noexcept
            {
                stdexec::set_value(std::move(receiver), std::move(result));
            };
        });
    }

    Engine::Result
    Engine::visitScene(scene::SceneInstanceId id, const std::function<Result(scene::SceneInstance&)>& visitor) noexcept
    {
        if (!impl_->context.execution().isMainThread())
        {
            return cxx::unexpected(execution::toError(execution::EExecutionError::WRONG_THREAD));
        }
        if (!visitor)
        {
            return cxx::unexpected(execution::toError(execution::EExecutionError::INVALID_ARGUMENT));
        }
        auto* entry = impl_->scenes.find({id.slot, id.generation});
        if (!entry)
        {
            return cxx::unexpected(error::Error{InvalidScene, {id.slot, id.generation}});
        }
        if ((*entry)->tasks.stopToken().stop_requested())
        {
            return invalidState();
        }
        return visitor(*(*entry)->scene);
    }

    exec::task<Engine::Result> Engine::removeScene(scene::SceneInstanceId id) noexcept
    {
        auto& execution = impl_->context.execution();
        if (!execution.isMainThread())
        {
            co_return cxx::unexpected(execution::toError(execution::EExecutionError::WRONG_THREAD));
        }
        // Never erase from within the caller's System or stage traversal.
        co_await stdexec::schedule(execution.mainScheduler());
        if (impl_->running && impl_->stop_requested)
        {
            co_return invalidState();
        }
        auto* entry = impl_->scenes.find({id.slot, id.generation});
        if (!entry)
        {
            co_return cxx::unexpected(error::Error{InvalidScene, {id.slot, id.generation}});
        }
        auto* session = entry->get();
        std::erase_if(impl_->fixed_ticks, [id](const auto& tick) noexcept { return tick.scene == id; });
        std::make_heap(impl_->fixed_ticks.begin(), impl_->fixed_ticks.end(), Impl::later);
        session->tasks.requestStop();
        co_return co_await stdexec::write_env(
            impl_->enqueueRetirement(impl_->retireScene(id)),
            stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
        );
    }

    void Engine::requestStop() noexcept
    {
        impl_->stop_requested = true;
        impl_->context.execution().wake();
    }

    Engine::Result Engine::run(EventPump pump_events, EventWait wait_events) noexcept
    {
        auto& execution = impl_->context.execution();
        if (impl_->running || impl_->stop_requested || !execution.isMainThread())
        {
            return invalidState();
        }
        // Reject invocation from an Execution callback before taking ownership of the outer loop.
        if (auto ready = execution.dispatch(); !ready)
        {
            return cxx::unexpected(execution::toError(ready.error()));
        }
        impl_->context.bindEngineStages(*this);
        impl_->running = true;
        cxx::scope_exit finished([this]() noexcept { impl_->running = false; });
        Result result;
        while (!impl_->stop_requested)
        {
            const auto epoch = execution.wakeEpoch();
            result = impl_->dispatchEvents(pump_events);
            if (!result || impl_->stop_requested)
            {
                break;
            }
            result = impl_->advanceFixed(Clock::now());
            if (!result || impl_->stop_requested)
            {
                break;
            }
            if (!impl_->hasContinuousWork())
            {
                impl_->waitForWork(epoch, impl_->nextDeadline(), wait_events);
                continue;
            }
            result = impl_->runStage(EEngineStage::BEGIN_UPDATE, pump_events, wait_events);
            if (result && !impl_->stop_requested)
            {
                result = impl_->advanceFixed(Clock::now());
            }
            if (result && !impl_->stop_requested)
            {
                result = impl_->advanceVariable(Clock::now());
            }
            if (result && !impl_->stop_requested)
            {
                result = impl_->runStage(EEngineStage::SIMULATION_FINISHED, pump_events, wait_events);
            }
            if (result && !impl_->stop_requested)
            {
                result = impl_->applyCommands();
                if (result)
                {
                    result = impl_->executeSystems();
                }
            }
            if (result && !impl_->stop_requested)
            {
                result = impl_->runStage(EEngineStage::END_UPDATE, pump_events, wait_events);
            }
            if (!result || impl_->stop_requested)
            {
                static_cast<void>(impl_->runStage(EEngineStage::UPDATE_ABORTED, pump_events, wait_events));
                break;
            }
        }
        auto settled = impl_->finish(pump_events, wait_events);
        return settled ? result : settled;
    }
} // namespace lux::engine

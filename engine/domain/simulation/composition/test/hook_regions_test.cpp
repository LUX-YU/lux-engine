#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>

#include <array>
#include <barrier>
#include <cassert>
#include <memory>
#include <thread>

namespace
{
    using namespace lux::simulation;
    constexpr lux::system::SystemInstanceId System{701U};
    constexpr HookPointId First{1U}, Stable{2U};
    constexpr EventPointId Samples{4U};
    constexpr SimulationTaskId Middle{2U}, Final{3U};
    struct Marker final { unsigned value{}; };
    struct FollowUp final { unsigned value{}; };
    struct NextFrame final { unsigned value{}; };

    struct State final
    {
        std::thread::id caller{std::this_thread::get_id()};
        std::barrier<>* rendezvous{};
        bool overflow{};
        bool fail_hook{};
        bool fail_command{};
        unsigned constructions{};
        unsigned value{};
        unsigned published{};
        unsigned callbacks{};
        unsigned stable_calls{};
        unsigned event_callbacks{};
        script::ScriptEventEndpointDescriptor event_descriptor;
        unsigned step{};
        ecs::Registry* registry{};
        ecs::Entity entity{ecs::NullEntity};
        SimulationCommandProducer* first_commands{};
        SimulationCommandProducer* stable_commands{};
        std::array<unsigned, 12U> order{};
        std::size_t count{};
        void record(unsigned point) noexcept { order[count++] = point; }
    };

    struct RegionSystem final
    {
        inline static constexpr auto Access = makeSystemAccessSpec<>();
        inline static constexpr std::array Stages{
            SimulationTaskSpec{PrimarySimulationTask, "produce"},
            SimulationTaskSpec{Middle, "continue"},
            SimulationTaskSpec{Final, "propagate"}
        };
        inline static constexpr std::array Hooks{
            makeHookPointSpec<void()>(First, "before_continue"),
            makeHookPointSpec<void()>(Stable, "stable_resume", true, true)
        };
        inline static constexpr std::array Events{makeEventPointSpec<std::int32_t>(
            Samples, "samples", Stable, EEventRoute::SIMULATION_BROADCAST, "lux.i32", 1U, true)};
        inline static constexpr SimulationSystemDescription Description{
            .type = {.canonical_name = "lux.test.execution.regions", .version = 1U},
            .hooks = Hooks,
            .events = Events,
            .tasks = Stages
        };
        explicit RegionSystem(State& value) noexcept
            : state(value), first_endpoint(System, First, first), stable_endpoint(System, Stable, stable)
        {
            ++state.constructions;
            assert(first.prepare(1U) == EEndpointMutationError::NONE);
            assert(stable.prepare(1U) == EEndpointMutationError::NONE);
            marker_connection = state.registry->on_construct<Marker>().connect<&RegionSystem::onMarker>(*this);
            follow_up_connection = state.registry->on_construct<FollowUp>().connect<&RegionSystem::onFollowUp>(*this);
        }
        void onMarker(ecs::Registry&, ecs::Entity entity) noexcept
        {
            if (state.step != 1U)
                return;
            auto writer = first_commands.begin();
            assert(writer && writer->emplace<FollowUp>(entity, FollowUp{42U}));
        }
        void onFollowUp(ecs::Registry&, ecs::Entity entity) noexcept
        {
            auto writer = stable_commands.begin();
            assert(writer && writer->emplace<NextFrame>(entity, NextFrame{99U}));
        }
        State& state;
        HookPoint<void()> first;
        HookPoint<void()> stable;
        script::ScriptHookEndpoint<void()> first_endpoint;
        script::ScriptHookEndpoint<void()> stable_endpoint;
        using Channel = HookChannel<SimulationBroadcastRoute, std::int32_t>;
        Channel* samples{};
        Channel::Producer first_writer;
        Channel::Producer middle_writer;
        std::optional<script::ScriptEventEndpoint<SimulationBroadcastRoute, std::int32_t>> event_endpoint;
        SimulationCommandProducer first_commands;
        SimulationCommandProducer stable_commands;
        entt::scoped_connection marker_connection;
        entt::scoped_connection follow_up_connection;
    };

    auto install(SimulationBuilder& builder, SimulationSystemView view) noexcept
        -> lux::cxx::expected<void, SimulationSystemBuildFailure>
    {
        auto system = builder.emplaceSystem<RegionSystem>(view.instanceId(), builder.registry().ctx().get<State>());
        if (!system)
            return lux::cxx::unexpected(system.error());
        auto channel = builder.createHookChannel<SimulationBroadcastRoute, std::int32_t>(
            view.instanceId(), Samples, {2U, 2U, 2U, 128U});
        if (!channel)
            return lux::cxx::unexpected(channel.error());
        // Reverse install order is not lane identity: the compiler assigns lanes by stable stage ID.
        auto middle_writer = builder.bindHookChannelProducer(view.instanceId(), Middle, **channel);
        auto first_writer = builder.bindHookChannelProducer(view.instanceId(), PrimarySimulationTask, **channel);
        if (!middle_writer || !first_writer)
            return lux::cxx::unexpected(!middle_writer ? middle_writer.error() : first_writer.error());
        assert(!builder.bindHookChannelProducer(view.instanceId(), PrimarySimulationTask, **channel));
        (*system)->samples = *channel;
        (*system)->event_endpoint.emplace(System, Samples, **channel);
        (*system)->state.event_descriptor = (*system)->event_endpoint->descriptor();
        auto published_event = builder.publishScriptEvent(view.instanceId(), (*system)->state.event_descriptor);
        if (!published_event)
            return published_event;
        (*system)->first_writer = *first_writer;
        (*system)->middle_writer = *middle_writer;
        assert(!first_writer->begin().record(90));
        assert(!(*channel)->begin(0U).record(91));
        auto first_commands = builder.prepareCommandProducer(SimulationExecutionPoint::hook(System, First), {8U, 256U});
        auto stable_commands = builder.prepareCommandProducer(
            SimulationExecutionPoint::hook(System, Stable), {8U, 256U});
        if (!first_commands || !stable_commands)
            return lux::cxx::unexpected(!first_commands ? first_commands.error() : stable_commands.error());
        (*system)->first_commands = *first_commands;
        (*system)->stable_commands = *stable_commands;
        (*system)->state.first_commands = &(*system)->first_commands;
        (*system)->state.stable_commands = &(*system)->stable_commands;
        assert(!first_commands->begin());
        auto result = builder.publishScriptHook(view.instanceId(), (*system)->first_endpoint.descriptor());
        if (!result)
            return result;
        result = builder.publishScriptHook(view.instanceId(), (*system)->stable_endpoint.descriptor());
        if (!result)
            return result;
        result = builder.addSystemTask<RegionSystem>(view.instanceId(), [](RegionSystem& system) noexcept {
            if (system.state.rendezvous != nullptr)
                system.state.rendezvous->arrive_and_wait();
            system.state.value = 1U;
            ++system.state.step;
            system.state.record(1U);
            auto writer = system.first_writer.begin();
            assert(writer.record(1));
            if (system.state.overflow)
            {
                assert(writer.record(2));
                // A void producer must propagate its lane failure without cooperation from its return type.
                assert(!writer.record(3));
            }
        });
        if (!result)
            return result;
        result = builder.addSystemTask<RegionSystem>(view.instanceId(), [](RegionSystem& system) noexcept {
            assert(system.state.value == 2U);
            assert(system.state.registry->all_of<Marker>(system.state.entity));
            assert(system.state.step == 1U ? !system.state.registry->all_of<FollowUp>(system.state.entity) :
                system.state.registry->all_of<NextFrame>(system.state.entity));
            system.state.value += 10U;
            system.state.record(5U);
            auto writer = system.middle_writer.begin();
            assert(writer.record(12));
        }, Middle);
        if (!result)
            return result;
        result = builder.addSystemTask<RegionSystem>(view.instanceId(), [](RegionSystem& system) noexcept {
            assert(system.state.value == 13U);
            assert(system.state.registry->all_of<FollowUp>(system.state.entity));
            assert(!system.state.registry->all_of<Marker>(system.state.entity));
            assert(system.state.step == 1U ? !system.state.registry->all_of<NextFrame>(system.state.entity) :
                system.state.registry->get<NextFrame>(system.state.entity).value == 99U);
            system.state.published = system.state.value;
            system.state.record(9U);
        }, Final);
        if (!result)
            return result;
        result = builder.addSystemHookTask<RegionSystem>(view.instanceId(), First,
            [](RegionSystem& system, const HookInvocation& invocation) noexcept {
                assert(std::this_thread::get_id() == system.state.caller);
                assert(!system.samples->beginOwner(invocation).record(90));
                system.state.record(3U);
                static_cast<void>(system.first.dispatch(invocation));
                return !system.state.fail_hook;
            });
        if (!result)
            return result;
        return builder.addSystemHookTask<RegionSystem>(view.instanceId(), Stable,
            [](RegionSystem& system, const HookInvocation& invocation) noexcept {
                assert(std::this_thread::get_id() == system.state.caller);
                assert(invocation.stableResume());
                const auto first = system.samples->lane(0U);
                // Having a descriptor is not permission to deliver Script during the direct-Hook phase.
                const auto endpoint = system.event_endpoint->descriptor();
                assert(endpoint.consume(endpoint.context) == 0U);
                const auto middle = system.samples->lane(1U);
                assert(first.size() == 1U && middle.size() == 1U);
                assert(first.front().payload == 1 && middle.front().payload == 12);
                const auto carried = system.samples->lane(2U);
                assert(invocation.clock().step_index == 1U ? carried.empty() : carried.front().payload == 99);
                system.state.record(7U);
                static_cast<void>(system.stable.dispatch(invocation));
                // A second native consumer still borrows exactly the same sealed backing spans.
                assert(system.samples->lane(0U).data() == first.data());
                assert(system.samples->lane(1U).front().payload == 12);
                assert(!system.samples->beginOwner().record(98));
                auto next = system.samples->beginOwner(invocation);
                assert(next.record(99));
            });
    }

    void run(std::barrier<>* rendezvous = nullptr, bool overflow = false,
        bool missing_producer = false, bool stale_hook_contract = false, bool fail_hook = false,
        bool fail_command = false)
    {
        ecs::Registry registry;
        auto& state = registry.ctx().emplace<State>();
        state.rendezvous = rendezvous;
        state.overflow = overflow;
        state.fail_hook = fail_hook;
        state.fail_command = fail_command;
        state.registry = &registry;
        state.entity = registry.create();
        SimulationDescriptionBuilder builder;
        auto authored = RegionSystem::Description;
        auto hooks = RegionSystem::Hooks;
        if (stale_hook_contract)
        {
            ++hooks[1].contract_version;
            authored.hooks = hooks;
        }
        assert(builder.addSystem(System, "regions", authored));
        if (!missing_producer)
            assert(builder.addChannelProducer({System, Samples, System, Middle}));
        assert(builder.addChannelProducer({System, Samples, System, PrimarySimulationTask}));
        using Point = SimulationExecutionPoint;
        const std::array points{Point::task(System), Point::hook(System, First),
            Point::task(System, Middle), Point::hook(System, Stable), Point::task(System, Final)};
        // Deliberately register constraints in reverse order; semantic order is unchanged.
        for (std::size_t index = points.size() - 1U; index != 0U; --index)
            assert(builder.addExecutionDependency(points[index - 1U], points[index]));
        auto description = std::move(builder).build();
        assert(description);
        SimulationSystemRegistry registrations;
        assert(registrations.add({lux::system::systemTypeId(RegionSystem::Description.type.canonical_name),
            lux::cxx::typeToken<RegionSystem>(), &RegionSystem::Description,
            RegionSystem::Access.spec(), {}, &install}));
        auto simulation = Simulation::create(registry,
            std::make_shared<SimulationDescription>(std::move(*description)), registrations);
        if (missing_producer || stale_hook_contract)
        {
            assert(!simulation && state.callbacks == 0U && state.count == 0U);
            if (stale_hook_contract)
                assert(state.constructions == 0U);
            return;
        }
        assert(simulation);
        const auto graph_stats = simulation->graphPreparationStats();
        assert(graph_stats.reachability_walks == 4U);
        assert(graph_stats.reachability_storage_bytes != 0U);
        const auto event = simulation->scriptEventEndpoints().front();
        assert(event.connect(event.context, &state, [](void* context, ecs::Entity, lux_script_call_frame&) noexcept {
            auto& state = *static_cast<State*>(context);
            ++state.event_callbacks;
            assert(state.event_descriptor.consume(state.event_descriptor.context) == 0U);
        }));
        for (const auto& endpoint : simulation->scriptHookEndpoints())
        {
            auto connected = endpoint.connect(endpoint.context, &state,
                [](void* context, lux_script_call_frame&) noexcept {
                    auto& state = *static_cast<State*>(context);
                    assert(std::this_thread::get_id() == state.caller);
                    ++state.value;
                    ++state.callbacks;
                    auto writer = (state.callbacks % 2U == 1U ? state.first_commands : state.stable_commands)->begin();
                    assert(writer);
                    if (state.callbacks % 2U == 1U)
                        assert(writer->emplace<Marker>(state.entity, Marker{41U}));
                    else
                        assert(writer->remove<Marker>(state.entity));
                    if (state.fail_command)
                        writer->destroy(ecs::NullEntity);
                });
            assert(connected);
        }
        auto binding = simulation->bindHookCallbacks({&state,
            [](void* context, const SimulationClockSnapshot&, bool stable) noexcept {
                auto& state = *static_cast<State*>(context);
                state.record(stable ? 6U : 2U);
                return true;
            },
            [](void* context, const SimulationClockSnapshot& clock, bool stable) noexcept {
                auto& state = *static_cast<State*>(context);
                if (stable)
                {
                    assert(clock.step_index == state.stable_calls + 1U);
                    ++state.stable_calls;
                }
                return true;
            },
            [](void* context, const SimulationClockSnapshot&) noexcept {
                auto& state = *static_cast<State*>(context);
                state.record(state.callbacks % 2U == 1U ? 4U : 8U);
                return true;
            }});
        assert(binding);
        auto executor = lux::task::TaskExecutor::create({2U, 16U});
        assert(executor);
        if (overflow)
        {
            assert(!simulation->execute(*executor, SimulationDuration{1}));
            assert(state.callbacks == 0U && state.stable_calls == 0U && state.published == 0U);
            return;
        }
        if (fail_hook)
        {
            const auto result = simulation->execute(*executor, SimulationDuration{1});
            assert(!result && result.error().code == ESimulationExecutionError::SYSTEM_TASK_FAILURE);
            assert(state.count == 3U && state.callbacks == 1U && state.published == 0U);
            return;
        }
        if (fail_command)
        {
            const auto result = simulation->execute(*executor, SimulationDuration{1});
            assert(!result && result.error().code == ESimulationExecutionError::ECS_COMMAND_FAILURE);
            assert(result.error().ecs_command.code == ecs::EEcsCommandError::INVALID_ENTITY);
            assert(result.error().ecs_command.producer == 0U && result.error().ecs_command.command == 1U);
            assert(state.callbacks == 1U && state.published == 0U);
            return;
        }
        for (unsigned step = 1U; step <= 2U; ++step)
        {
            state.count = 0U;
            assert(simulation->execute(*executor, SimulationDuration{1}));
            assert(state.callbacks == 2U * step && state.stable_calls == step && state.published == 13U);
            assert(state.event_callbacks == (step == 1U ? 2U : 5U));
            assert(step == 1U ? !registry.all_of<NextFrame>(state.entity) : registry.all_of<NextFrame>(state.entity));
            assert(state.count == 9U);
            for (std::size_t index{}; index < state.count; ++index)
                assert(state.order[index] == index + 1U);
        }
    }
}

int main()
{
    run();
    run(nullptr, true);
    run(nullptr, false, true);
    run(nullptr, false, false, true);
    run(nullptr, false, false, false, true);
    run(nullptr, false, false, false, false, true);
    std::barrier rendezvous{2};
    std::jthread first([&] { run(&rendezvous); });
    std::jthread second([&] { run(&rendezvous); });
}

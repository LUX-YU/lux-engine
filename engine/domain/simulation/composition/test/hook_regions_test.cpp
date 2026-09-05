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
    constexpr SimulationTaskId Middle{2U}, Final{3U};

    struct State final
    {
        std::thread::id caller{std::this_thread::get_id()};
        std::barrier<>* rendezvous{};
        unsigned value{};
        unsigned published{};
        unsigned callbacks{};
        unsigned stable_calls{};
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
        inline static constexpr SimulationSystemDescription Description{
            .type = {.canonical_name = "lux.test.execution.regions", .version = 1U},
            .hooks = Hooks,
            .tasks = Stages
        };
        explicit RegionSystem(State& value) noexcept
            : state(value), first_endpoint(System, First, first), stable_endpoint(System, Stable, stable)
        {
            assert(first.prepare(1U) == EEndpointMutationError::NONE);
            assert(stable.prepare(1U) == EEndpointMutationError::NONE);
        }
        State& state;
        HookPoint<void()> first;
        HookPoint<void()> stable;
        script::ScriptHookEndpoint<void()> first_endpoint;
        script::ScriptHookEndpoint<void()> stable_endpoint;
    };

    auto install(SimulationBuilder& builder, SimulationSystemView view) noexcept
        -> lux::cxx::expected<void, SimulationSystemBuildFailure>
    {
        auto system = builder.emplaceSystem<RegionSystem>(view.instanceId(), builder.registry().ctx().get<State>());
        if (!system)
            return lux::cxx::unexpected(system.error());
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
            system.state.record(1U);
        });
        if (!result)
            return result;
        result = builder.addSystemTask<RegionSystem>(view.instanceId(), [](RegionSystem& system) noexcept {
            assert(system.state.value == 2U);
            system.state.value += 10U;
            system.state.record(5U);
        }, Middle);
        if (!result)
            return result;
        result = builder.addSystemTask<RegionSystem>(view.instanceId(), [](RegionSystem& system) noexcept {
            assert(system.state.value == 13U);
            system.state.published = system.state.value;
            system.state.record(9U);
        }, Final);
        if (!result)
            return result;
        result = builder.addSystemHookTask<RegionSystem>(view.instanceId(), First,
            [](RegionSystem& system, const HookInvocation& invocation) noexcept {
                assert(std::this_thread::get_id() == system.state.caller);
                system.state.record(3U);
                static_cast<void>(system.first.dispatch(invocation));
            });
        if (!result)
            return result;
        return builder.addSystemHookTask<RegionSystem>(view.instanceId(), Stable,
            [](RegionSystem& system, const HookInvocation& invocation) noexcept {
                assert(std::this_thread::get_id() == system.state.caller);
                assert(invocation.stableResume());
                system.state.record(7U);
                static_cast<void>(system.stable.dispatch(invocation));
            });
    }

    void run(std::barrier<>* rendezvous = nullptr)
    {
        ecs::Registry registry;
        auto& state = registry.ctx().emplace<State>();
        state.rendezvous = rendezvous;
        SimulationDescriptionBuilder builder;
        assert(builder.addSystem(System, "regions", RegionSystem::Description));
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
        assert(simulation);
        for (const auto& endpoint : simulation->scriptHookEndpoints())
        {
            auto connected = endpoint.connect(endpoint.context, &state,
                [](void* context, lux_script_call_frame&) noexcept {
                    auto& state = *static_cast<State*>(context);
                    assert(std::this_thread::get_id() == state.caller);
                    ++state.value;
                    ++state.callbacks;
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
                    assert(clock.step_index == 1U);
                    ++state.stable_calls;
                }
                return true;
            },
            [](void* context, const SimulationClockSnapshot&) noexcept {
                auto& state = *static_cast<State*>(context);
                state.record(state.callbacks == 1U ? 4U : 8U);
                return true;
            }});
        assert(binding);
        auto executor = lux::task::TaskExecutor::create({2U, 16U});
        assert(executor && simulation->execute(*executor, SimulationDuration{1}));
        assert(state.callbacks == 2U && state.stable_calls == 1U && state.published == 13U);
        assert(state.count == 9U);
        for (std::size_t index{}; index < state.count; ++index)
            assert(state.order[index] == index + 1U);
    }
}

int main()
{
    run();
    std::barrier rendezvous{2};
    std::jthread first([&] { run(&rendezvous); });
    std::jthread second([&] { run(&rendezvous); });
}

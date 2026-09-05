#pragma once
#include <lux/engine/physics2d/Physics2DSystem.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>

namespace installed_consumer
{
    using namespace lux::simulation;
    inline constexpr lux::system::SystemInstanceId PhysicsId{71U};
    inline constexpr lux::system::SystemInstanceId ProbeId{72U};
    inline constexpr HookPointId TickHook{73U};
    inline constexpr EventPointId PulseEvent{74U};
    inline constexpr std::array ProbeHooks{makeHookPointSpec<void()>(TickHook, "tick", true, true)};
    inline constexpr std::array ProbeEvents{makeEventPointSpec<std::int32_t>(
        PulseEvent, "pulse", TickHook, EEventRoute::SIMULATION_BROADCAST, "lux.i32", 1U)};
    inline constexpr SimulationSystemDescription ProbeDescription{
        .type = {.canonical_name = "installed.CoroutineProbe", .version = 1U},
        .hooks = ProbeHooks, .events = ProbeEvents
    };

    inline lux::cxx::expected<SimulationDescription, SimulationDescriptionFailure> makeDescription() noexcept
    {
        lux::physics2d::Physics2DSystemConfiguration physics_configuration;
        physics_configuration.gravity_y = 0.0;
        physics_configuration.body_capacity = 8U;
        const auto encoded = lux::physics2d::makePhysics2DSystemConfiguration(physics_configuration);
        if (!encoded) return lux::cxx::unexpected(SimulationDescriptionFailure{});
        SimulationDescriptionBuilder builder;
        auto added = builder.addSystem(PhysicsId, "physics", lux::physics2d::physics2DSystemDescription(), *encoded);
        if (!added) return lux::cxx::unexpected(added.error());
        added = builder.addSystem(ProbeId, "probe", ProbeDescription);
        if (!added) return lux::cxx::unexpected(added.error());
        for (auto producer : {PhysicsId, ProbeId})
        {
            added = builder.addExecutionDependency(SimulationExecutionPoint::task(producer),
                SimulationExecutionPoint::hook(ProbeId, TickHook));
            if (!added) return lux::cxx::unexpected(added.error());
        }
        added = builder.addChannelProducer({ProbeId, PulseEvent, ProbeId, PrimarySimulationTask});
        if (!added) return lux::cxx::unexpected(added.error());
        return std::move(builder).build();
    }
}

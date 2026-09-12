#pragma once
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
namespace installed_consumer
{
using namespace lux::simulation;
inline constexpr lux::system::SystemInstanceId ProbeId{72U};
inline constexpr HookPointId TickHook{73U};
inline constexpr EventPointId PulseEvent{74U};
inline constexpr std::array ProbeHooks{makeHookPointSpec<void()>(TickHook, "tick", true, true)};
inline constexpr std::array ProbeEvents{makeEventPointSpec<std::int32_t>(
    PulseEvent, "pulse", TickHook, EEventRoute::SIMULATION_BROADCAST, "lux.i32", 1U)};
inline constexpr SimulationSystemDescription ProbeDescription{
    .type = {.canonical_name = "installed.NativeLuaTaskProbe", .version = 1U}, .hooks = ProbeHooks, .events = ProbeEvents};
inline lux::cxx::expected<SimulationDescription, SimulationDescriptionFailure> makeDescription() noexcept
{
    SimulationDescriptionBuilder builder;
    auto result = builder.addSystem(ProbeId, "probe", ProbeDescription);
    if (!result) return lux::cxx::unexpected(result.error());
    result = builder.addExecutionDependency(SimulationExecutionPoint::task(ProbeId),
        SimulationExecutionPoint::hook(ProbeId, TickHook));
    if (!result) return lux::cxx::unexpected(result.error());
    result = builder.addChannelProducer({ProbeId, PulseEvent, ProbeId, PrimarySimulationTask});
    if (!result) return lux::cxx::unexpected(result.error());
    return std::move(builder).build();
}
}

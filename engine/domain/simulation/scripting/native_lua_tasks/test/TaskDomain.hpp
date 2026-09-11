#pragma once
#include <array>
#include <cassert>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
namespace lux::simulation::na1
{
inline constexpr lux::system::SystemInstanceId System{0x4e4131U};
inline constexpr HookPointId Hook{0x4e4132U};
inline constexpr EventPointId Event{0x4e4133U};
inline SimulationDescription domain()
{
    constexpr std::array hooks{makeHookPointSpec<void()>(Hook, "task")};
    constexpr std::array events{
        makeEventPointSpec<std::int32_t>(Event, "event", Hook, EEventRoute::SIMULATION_BROADCAST, "lux.i32", 1U)};
    const SimulationSystemDescription system{
        .type = {.canonical_name = "lux.na1.task", .version = 1U}, .hooks = hooks, .events = events};
    SimulationDescriptionBuilder builder;
    assert(builder.addSystem(System, "task", system));
    auto result = std::move(builder).build();
    assert(result);
    return std::move(*result);
}
} // namespace lux::simulation::na1

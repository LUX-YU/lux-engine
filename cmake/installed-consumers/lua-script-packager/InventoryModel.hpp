#pragma once

#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/scripting/ScriptEventSource.hpp>

#include <array>

namespace installed_consumer
{
    inline constexpr lux::system::SystemInstanceId InventorySystemId{51U};
    inline constexpr lux::simulation::EventPointId ChangedEvent{52U};
    inline constexpr lux::simulation::HookPointId TickHook{53U};
    inline constexpr std::array InventoryHooks{
        lux::simulation::makeHookPointSpec<void()>(TickHook, "tick", true, true)};
    inline constexpr std::array InventoryEvents{lux::simulation::makeEventPointSpec<std::int32_t>(
        ChangedEvent, "changed", TickHook, lux::simulation::EEventRoute::SIMULATION_BROADCAST, "lux.i32", 1U)};
    inline constexpr lux::simulation::SimulationSystemDescription InventoryDescription{
        .type = {.canonical_name = "consumer.inventory.system", .version = 1U},
        .hooks = InventoryHooks, .events = InventoryEvents};

    inline auto inventoryDescription()
    {
        using namespace lux::simulation;
        SimulationDescriptionBuilder builder;
        auto added = builder.addSystem(InventorySystemId, "Inventory", InventoryDescription);
        if (added)
            added = builder.addExecutionDependency(SimulationExecutionPoint::task(InventorySystemId),
                SimulationExecutionPoint::hook(InventorySystemId, TickHook));
        if (added)
            added = builder.addChannelProducer({
                InventorySystemId, ChangedEvent, InventorySystemId, PrimarySimulationTask});
        if (!added)
            return lux::cxx::expected<SimulationDescription, SimulationDescriptionFailure>{
                lux::cxx::unexpected(added.error())};
        return std::move(builder).build();
    }
}

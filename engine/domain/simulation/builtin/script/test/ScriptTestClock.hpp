#pragma once

#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <cassert>
#include <memory>
#include <optional>

namespace lux::simulation::script::test
{
    // The same empty Simulation clock owner used by the runtime benchmark; no fabricated step mutation.
    class ScriptTestClock final
    {
    public:
        explicit ScriptTestClock(ecs::Registry& registry)
        {
            SimulationDescriptionBuilder builder;
            auto description = std::move(builder).build();
            assert(description);
            SimulationSystemRegistry types;
            auto simulation = Simulation::create(registry,
                std::make_shared<SimulationDescription>(std::move(*description)), types);
            assert(simulation);
            simulation_.emplace(std::move(*simulation));
            assert(simulation_->seal());
            auto executor = task::TaskExecutor::create({0U, 8U});
            assert(executor);
            executor_.emplace(std::move(*executor));
        }
        [[nodiscard]] const SimulationClock& clock() const noexcept { return simulation_->clock(); }
        void advance(SimulationDuration delta)
        {
            assert(simulation_->execute(*executor_, delta));
        }
    private:
        std::optional<Simulation> simulation_;
        std::optional<task::TaskExecutor> executor_;
    };
}

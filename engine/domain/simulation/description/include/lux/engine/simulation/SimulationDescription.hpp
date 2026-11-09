#pragma once

#include <lux/engine/simulation/TickGroupDescription.hpp>
#include <vector>

namespace lux::simulation
{
    // Assembly data only. Both group order and each group's system order are significant.
    // Runtime graphs, deadlines and asynchronous work are reconstructed by their actual owners.
    struct SimulationDescription final
    {
        std::vector<TickGroupDescription> groups;
    };
} // namespace lux::simulation

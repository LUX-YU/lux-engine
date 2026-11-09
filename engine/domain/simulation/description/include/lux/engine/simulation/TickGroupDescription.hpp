#pragma once

#include <lux/engine/simulation/SimulationSystemDescription.hpp>
#include <lux/engine/simulation/SimulationTime.hpp>

#include <optional>
#include <vector>

namespace lux::simulation
{
    // No fixed step means one execution per admitted Engine update. Fixed steps must be positive.
    // System insertion order is preserved when constructing read/write dependency edges.
    struct TickGroupDescription final
    {
        std::optional<SimulationDuration>        fixed_step;
        std::vector<SimulationSystemDescription> systems;
    };
} // namespace lux::simulation

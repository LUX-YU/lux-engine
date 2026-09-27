#pragma once

#include <chrono>
#include <cstdint>

namespace lux::simulation
{
    using SimulationDuration = std::chrono::nanoseconds;

    struct SimulationTime final
    {
        SimulationDuration elapsed{};
        SimulationDuration delta{};
        std::uint64_t step_index{};
    };
}

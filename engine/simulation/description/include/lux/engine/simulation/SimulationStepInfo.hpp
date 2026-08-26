#pragma once

#include <cstdint>

namespace lux::simulation
{
    struct SimulationStepInfo final
    {
        float delta_seconds{};
        std::uint64_t step_index{};
    };
}

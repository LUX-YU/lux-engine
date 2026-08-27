#pragma once

#include <lux/engine/function/script/ScriptSemantic.hpp>

#include <cstdint>

namespace lux::simulation
{
    struct SimulationStepInfo final
    {
        float delta_seconds{};
        std::uint64_t step_index{};
    };
}

namespace lux::script
{
    template <> struct ScriptSemanticTypeTraits<lux::simulation::SimulationStepInfo> final
    {
        inline static constexpr std::string_view CanonicalName = "lux.simulation.SimulationStepInfo";
    };
}

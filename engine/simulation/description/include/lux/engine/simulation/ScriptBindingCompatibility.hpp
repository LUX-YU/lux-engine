#pragma once

#include <lux/engine/description/Script.hpp>
#include <lux/engine/simulation/ScriptMountDescription.hpp>
#include <lux/engine/simulation/SimulationDescription.hpp>

#include <cstdint>

namespace lux::simulation
{
    enum class EScriptBindingCompatibility : std::uint8_t
    {
        COMPATIBLE,
        INVALID_FUNCTION,
        TARGET_NOT_FOUND,
        TARGET_AMBIGUOUS,
        TARGET_TYPE_MISMATCH,
        SCOPE_MISMATCH,
        CARDINALITY_MISMATCH,
        SIGNATURE_MISMATCH,
    };

    [[nodiscard]] LUX_ENGINE_SIMULATION_DESCRIPTION_PUBLIC
    EScriptBindingCompatibility evaluateScriptBindingCompatibility(
        const SimulationDescription& simulation,
        lux::rdesc::EScriptModel model,
        const lux::rdesc::ScriptFunction& function,
        const ScriptBindingTarget& target
    ) noexcept;
}

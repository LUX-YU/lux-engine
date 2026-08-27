#pragma once

#include <lux/engine/core/semantic/SemanticType.hpp>
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

namespace lux::semantic
{
    template <>
    struct TypeTraits<lux::simulation::SimulationStepInfo> final
    {
        inline static constexpr std::string_view CanonicalName =
            "lux.simulation.SimulationStepInfo";
        inline static constexpr std::uint8_t AbiKind =
            static_cast<std::uint8_t>(EAbiKind::STRUCT_REF);
        inline static constexpr std::uint32_t Size =
            sizeof(lux::simulation::SimulationStepInfo);
        inline static constexpr std::uint32_t Alignment =
            alignof(lux::simulation::SimulationStepInfo);
    };
}

namespace lux::script
{
    template <>
    struct ScriptSemanticTypeTraits<lux::simulation::SimulationStepInfo> final
    {
        inline static constexpr std::string_view CanonicalName =
            lux::semantic::TypeTraits<
                lux::simulation::SimulationStepInfo>::CanonicalName;
        inline static constexpr std::uint8_t AbiKind =
            lux::semantic::TypeTraits<
                lux::simulation::SimulationStepInfo>::AbiKind;
        inline static constexpr std::uint32_t Size =
            lux::semantic::TypeTraits<
                lux::simulation::SimulationStepInfo>::Size;
        inline static constexpr std::uint32_t Alignment =
            lux::semantic::TypeTraits<
                lux::simulation::SimulationStepInfo>::Alignment;
    };
}

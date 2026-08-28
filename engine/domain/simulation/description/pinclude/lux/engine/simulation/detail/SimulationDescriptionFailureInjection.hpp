#pragma once

#include <lux/engine/simulation/description/visibility.h>

#include <cstdint>

namespace lux::simulation::detail
{
    enum class ESimulationDescriptionFailurePoint : std::uint8_t
    {
        MUTATION_ALLOCATION,
        BUILD_ALLOCATION,
        BUILD_SIZE_OVERFLOW,
    };

    LUX_ENGINE_SIMULATION_DESCRIPTION_PUBLIC void
    failNextSimulationDescriptionOperationForTest(ESimulationDescriptionFailurePoint point) noexcept;

    [[nodiscard]] bool consumeSimulationDescriptionFailureForTest(ESimulationDescriptionFailurePoint point) noexcept;
}

#pragma once

#include <lux/engine/simulation/description/visibility.h>

#include <lux/cxx/core/StableNameId.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace lux::simulation
{
    struct SimulationDataSchemaId final
    {
        std::uint64_t hash{};
        std::string name;

        [[nodiscard]] bool valid() const noexcept
        {
            return !name.empty() && hash == lux::cxx::Fnv1a64::hash(name);
        }

        friend bool operator==(
            const SimulationDataSchemaId&,
            const SimulationDataSchemaId&
        ) noexcept = default;
    };

    struct SimulationDataSchemaIdLess final
    {
        [[nodiscard]] bool operator()(
            const SimulationDataSchemaId& left,
            const SimulationDataSchemaId& right
        ) const noexcept
        {
            return left.hash < right.hash ||
                (left.hash == right.hash && left.name < right.name);
        }
    };

    [[nodiscard]] LUX_ENGINE_SIMULATION_DESCRIPTION_PUBLIC
    SimulationDataSchemaId simulationDataSchemaId(std::string_view name);
}

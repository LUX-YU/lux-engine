#include <lux/engine/simulation/SimulationDataSchemaId.hpp>

namespace lux::simulation
{
    SimulationDataSchemaId simulationDataSchemaId(std::string_view name)
    {
        return SimulationDataSchemaId{
            lux::cxx::Fnv1a64::hash(name),
            std::string(name)};
    }
}

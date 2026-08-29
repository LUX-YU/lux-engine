#include <lux/engine/simulation/SystemRegistry.hpp>

namespace
{
    lux::cxx::expected<void, lux::simulation::SystemBuildFailure> installProbe(
        lux::simulation::SimulationBuilder&,
        lux::simulation::SimulationSystemView
    ) noexcept
    {
        return {};
    }
}

int main()
{
    lux::simulation::SystemRegistry systems;
    const lux::simulation::SystemRegistration registration{
        lux::simulation::systemTypeId("lux.consumer.simulation-system"),
        1U,
        &installProbe
    };
    if (!systems.add(registration))
        return 1;
    const auto* found = systems.find(registration.type);
    return found != nullptr && found->version == 1U && systems.size() == 1U ? 0 : 2;
}

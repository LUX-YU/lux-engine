#include <lux/engine/simulation/SimulationSystemRegistry.hpp>

namespace
{
    struct ProbeSystem final
    {
        inline static constexpr auto Access = lux::simulation::makeSystemAccessSpec<>();
        inline static constexpr lux::simulation::SimulationSystemDescription Description{
            .type = {.canonical_name = "lux.consumer.simulation-system", .version = 1U}
        };
    };

    lux::cxx::expected<void, lux::simulation::SimulationSystemBuildFailure> installProbe(
        lux::simulation::SimulationBuilder&,
        lux::simulation::SimulationSystemView
    ) noexcept
    {
        return {};
    }
}

int main()
{
    lux::simulation::SimulationSystemRegistry systems;
    const lux::simulation::SimulationSystemRegistration registration{
        .type = lux::system::systemTypeId(ProbeSystem::Description.type.canonical_name),
        .cpp_type = lux::cxx::typeToken<ProbeSystem>(),
        .description = &ProbeSystem::Description,
        .access = ProbeSystem::Access.spec(),
        .configuration = lux::serialization::noPortableValueCodec(),
        .install = &installProbe
    };
    if (!systems.add(registration))
        return 1;
    const auto* found = systems.find(registration.type);
    return found != nullptr && found->description->type.version == 1U && systems.size() == 1U ? 0 : 2;
}

#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>

#include <utility>

int
main()
{
    lux::simulation::SimulationDescriptionBuilder builder;
    auto description = std::move(builder).build();
    return description && description->empty() ? 0 : 1;
}

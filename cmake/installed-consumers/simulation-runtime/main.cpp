#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>

#include <memory>
#include <utility>

int main()
{
    lux::simulation::SimulationDescriptionBuilder description_builder;
    auto description = std::move(description_builder).build();
    if (!description)
        return 1;

    auto shared_description =
        std::make_shared<lux::simulation::SimulationDescription>(std::move(*description));
    lux::simulation::ecs::Registry registry;
    lux::simulation::SystemRegistry system_types;
    auto simulation = lux::simulation::Simulation::create(
        registry,
        std::move(shared_description),
        system_types
    );
    if (!simulation)
        return 2;

    auto executor = lux::task::TaskExecutor::create({0U, 0U});
    return executor && simulation->execute(*executor) ? 0 : 3;
}

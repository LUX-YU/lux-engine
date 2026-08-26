#include <lux/engine/simulation/ecs/Registry.hpp>

int main()
{
    lux::simulation::ecs::Registry registry;
    const auto entity = registry.create();
    return registry.valid(entity) ? 0 : 1;
}

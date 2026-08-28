#include <lux/engine/simulation/ecs/EcsState.hpp>

int
main()
{
    lux::simulation::ecs::EcsState world;
    return world.edit() ? 0 : 1;
}

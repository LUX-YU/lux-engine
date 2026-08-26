#include <lux/engine/ecs/EcsState.hpp>

int main()
{
    lux::ecs::EcsState world;
    return world.edit() ? 0 : 1;
}

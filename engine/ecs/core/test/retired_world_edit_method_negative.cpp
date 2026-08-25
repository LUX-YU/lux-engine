#include <lux/engine/ecs/World.hpp>

int main()
{
    lux::ecs::World world;
    return world.edit() ? 0 : 1;
}

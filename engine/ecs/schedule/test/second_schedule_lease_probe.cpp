#include <lux/engine/ecs/Schedule.hpp>

int main()
{
    lux::ecs::World world;
    lux::ecs::Schedule first(world);
    lux::ecs::Schedule forbidden(world);
}

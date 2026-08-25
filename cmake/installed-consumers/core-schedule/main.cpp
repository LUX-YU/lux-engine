#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/World.hpp>

int main()
{
    lux::ecs::World world;
    lux::ecs::Schedule schedule(world);
    schedule.run(0.0F, 0U);
}

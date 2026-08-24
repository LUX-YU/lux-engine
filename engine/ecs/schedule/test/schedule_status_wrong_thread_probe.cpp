#include <lux/engine/ecs/Schedule.hpp>

#include <thread>

int main()
{
    lux::ecs::World world;
    lux::ecs::Schedule schedule(world);
    std::thread foreign([&schedule]
    {
        (void)schedule.closeComplete();
    });
    foreign.join();
}

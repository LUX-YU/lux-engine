#include <lux/engine/ecs/World.hpp>

struct world_mutable_get_negative final
{
    int value{};
};

int main()
{
    lux::ecs::World world;
    world.get<world_mutable_get_negative>(lux::ecs::NullEntity).value = 1;
}

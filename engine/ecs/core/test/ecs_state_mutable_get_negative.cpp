#include <lux/engine/ecs/EcsState.hpp>

struct world_mutable_get_negative final
{
    int value{};
};

int main()
{
    lux::ecs::EcsState world;
    world.get<world_mutable_get_negative>(lux::ecs::NullEntity).value = 1;
}

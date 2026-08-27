#include <lux/engine/simulation/ecs/EcsState.hpp>

struct world_mutable_get_negative final
{
    int value{};
};

int
main()
{
    lux::simulation::ecs::EcsState world;
    world.get<world_mutable_get_negative>(lux::simulation::ecs::NullEntity).value = 1;
}

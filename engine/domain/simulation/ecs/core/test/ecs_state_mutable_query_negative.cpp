#include <lux/engine/simulation/ecs/EcsState.hpp>

struct world_mutable_query_negative final
{
    int value{};
};

int
main()
{
    const lux::simulation::ecs::EcsState world;
    (void)world.query<lux::simulation::ecs::Write<world_mutable_query_negative>>();
}

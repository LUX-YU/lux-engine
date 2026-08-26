#include <lux/engine/ecs/EcsState.hpp>

struct world_mutable_query_negative final
{
    int value{};
};

int main()
{
    const lux::ecs::EcsState world;
    (void)world.query<lux::ecs::Write<world_mutable_query_negative>>();
}

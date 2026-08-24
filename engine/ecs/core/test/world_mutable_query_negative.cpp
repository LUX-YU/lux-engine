#include <lux/engine/ecs/World.hpp>

struct world_mutable_query_negative final
{
    int value{};
};

int main()
{
    const lux::ecs::World world;
    (void)world.query<lux::ecs::Write<world_mutable_query_negative>>();
}

#include <lux/engine/simulation/ecs/HierarchyIndex.hpp>

int main()
{
    lux::simulation::ecs::EcsState world;
    lux::simulation::ecs::HierarchyIndex hierarchy(world);
    auto edit = world.mutate();
    return lux::simulation::ecs::setParent(
        *edit,
        hierarchy,
        lux::simulation::ecs::NullEntity,
        lux::simulation::ecs::NullEntity
    ) ? 0 : 1;
}

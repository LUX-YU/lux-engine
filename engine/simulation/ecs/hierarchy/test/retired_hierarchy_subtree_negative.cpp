#include <lux/engine/simulation/ecs/HierarchyIndex.hpp>

int main()
{
    lux::simulation::ecs::EcsState world;
    lux::simulation::ecs::HierarchyIndex hierarchy(world);
    return static_cast<int>(hierarchy.subtree(lux::simulation::ecs::NullEntity).size());
}

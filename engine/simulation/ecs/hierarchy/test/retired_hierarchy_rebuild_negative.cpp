#include <lux/engine/simulation/ecs/HierarchyIndex.hpp>

int
main()
{
    lux::simulation::ecs::EcsState world;
    lux::simulation::ecs::HierarchyIndex hierarchy(world);
    return hierarchy.rebuild() ? 0 : 1;
}

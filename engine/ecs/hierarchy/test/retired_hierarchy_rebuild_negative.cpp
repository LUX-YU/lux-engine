#include <lux/engine/ecs/HierarchyIndex.hpp>

int main()
{
    lux::ecs::EcsState world;
    lux::ecs::HierarchyIndex hierarchy(world);
    return hierarchy.rebuild() ? 0 : 1;
}

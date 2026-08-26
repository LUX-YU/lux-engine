#include <lux/engine/ecs/HierarchyIndex.hpp>

int main()
{
    lux::ecs::EcsState world;
    lux::ecs::HierarchyIndex hierarchy(world);
    return static_cast<int>(hierarchy.subtree(lux::ecs::NullEntity).size());
}

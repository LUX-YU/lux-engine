#include <lux/engine/ecs/HierarchyIndex.hpp>

int main()
{
    lux::ecs::World world;
    lux::ecs::HierarchyIndex hierarchy(world);
    return static_cast<int>(hierarchy.subtree(lux::ecs::NullEntity).size());
}

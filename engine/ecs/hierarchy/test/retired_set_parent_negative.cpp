#include <lux/engine/ecs/HierarchyIndex.hpp>

int main()
{
    lux::ecs::World world;
    lux::ecs::HierarchyIndex hierarchy(world);
    auto edit = world.edit();
    return lux::ecs::setParent(
        *edit,
        hierarchy,
        lux::ecs::NullEntity,
        lux::ecs::NullEntity
    ) ? 0 : 1;
}

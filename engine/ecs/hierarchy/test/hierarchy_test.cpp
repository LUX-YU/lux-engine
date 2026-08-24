#include <lux/engine/ecs/HierarchyIndex.hpp>
#include <lux/engine/ecs/HierarchySchema.hpp>

#include <cassert>
#include <utility>

int main()
{
    lux::ecs::World world;
    lux::ecs::HierarchyIndex hierarchy{world};
    auto edit_result = world.edit();
    assert(edit_result);
    auto edit = std::move(*edit_result);
    const auto root = edit.create();
    const auto child = edit.create();
    const auto grandchild = edit.create();
    assert(lux::ecs::setParent(edit, hierarchy, child, root));
    assert(lux::ecs::setParent(edit, hierarchy, grandchild, child));
    assert(!lux::ecs::setParent(edit, hierarchy, root, grandchild));
    assert(hierarchy.parent(grandchild) == child);
    assert(hierarchy.subtree(root).size() == 3);
    assert(lux::ecs::clearParent(edit, hierarchy, child));
    assert(hierarchy.parent(child) == lux::ecs::NullEntity);
    edit = {};

    assert(hierarchy.rebuild());
    assert(hierarchy.preorder().size() == 2);
    assert(hierarchy.parent(child) == lux::ecs::NullEntity);
    assert(hierarchy.parent(grandchild) == child);
    assert(lux::ecs::hierarchyComponentSchemas().size() == 1);
}

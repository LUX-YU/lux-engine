#include <cassert>
#include <cstdio>
#include <lux/engine/simulation/ecs/hierarchy/detail/HierarchyMaintenance.hpp>
#include <string_view>

int main(int argc, char **argv)
{
    using namespace lux::simulation::ecs;
    Registry registry;
    HierarchyIndex hierarchy;
    HierarchyDeltaBatch deltas;
    detail::HierarchyMaintenance maintenance(registry, hierarchy, deltas);
    assert(deltas.prepare(32) && maintenance.prepare(32));
    auto parent = registry.create();
    const auto child = registry.create();
    registry.emplace<Parent>(child, parent);
    assert(maintenance.update());
    assert(hierarchy.parent(child) == parent);

    const auto restored = registry.create();
    registry.emplace<Parent>(restored); // Codec installation may then resolve the reference.
    registry.patch<Parent>(restored, [parent](Parent &value) { value.entity = parent; });
    const auto resolved = maintenance.update();
    if (argc == 2 && std::string_view(argv[1]) == "--expect-null")
    {
        assert(!resolved && resolved.error() == EHierarchyError::INVALID_ENTITY);
        assert(registry.get<Parent>(restored).entity == parent);
        std::puts("REPRODUCED: transient null Parent rejected after the same batch resolves a valid reference");
        return 0;
    }
    assert(resolved && hierarchy.parent(restored) == parent);
    registry.destroy(restored);
    assert(maintenance.update());

    for (unsigned index{}; index != 16; ++index)
    {
        const auto previous = parent;
        registry.destroy(parent);
        parent = registry.create();
        assert(parent != previous && entt::to_entity(parent) == entt::to_entity(previous));
        registry.patch<Parent>(child, [parent](Parent &value) { value.entity = parent; });
        const auto updated = maintenance.update();
        if (argc == 2 && std::string_view(argv[1]) == "--expect-stale")
        {
            assert(!updated && updated.error() == EHierarchyError::INVALID_ENTITY);
            assert(registry.valid(parent) && registry.get<Parent>(child).entity == parent);
            std::puts("REPRODUCED: valid replacement parent rejected by stale hierarchy generation");
            return 0;
        }
        assert(updated && hierarchy.synchronized());
        assert(hierarchy.parent(child) == parent);
        assert(hierarchy.children(previous).empty());
        assert(*hierarchy.children(parent).begin() == child);
    }
    registry.destroy(child);
    registry.destroy(parent);
    assert(maintenance.update() && hierarchy.size() == 0);
    std::puts("PASS hierarchy: parent slot reused sixteen times between maintenance turns, current child retained");
}

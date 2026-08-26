#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include <lux/engine/simulation/ecs/HierarchyIndex.hpp>
#include <lux/engine/simulation/ecs/hierarchy/detail/HierarchyMaintenance.hpp>

int main()
{
    using namespace lux::simulation::ecs;

    Registry registry;
    const Entity first_parent = registry.create();
    const Entity second_parent = registry.create();
    const Entity child = registry.create();
    registry.emplace<Parent>(child, first_parent);

    HierarchyIndex hierarchy;
    HierarchyDeltaBatch deltas;
    assert(deltas.prepare(16U));
    detail::HierarchyMaintenance maintenance(registry, hierarchy, deltas);
    assert(maintenance.prepare(16U));

    // Existing components are folded in after signal connection.
    assert(maintenance.update());
    assert(hierarchy.synchronized());
    assert(hierarchy.parent(child) == first_parent);
    assert(hierarchy.size() == 2U);
    assert(!deltas.exact());

    deltas.reset();
    assert(reparent(registry, child, second_parent));
    assert(maintenance.update());
    assert(hierarchy.parent(child) == second_parent);
    assert(deltas.exact());
    assert(deltas.values().size() == 1U);
    assert(deltas.values().front().kind == EHierarchyDeltaKind::REPARENTED);

    assert(detach(registry, child));
    assert(maintenance.update());
    assert(hierarchy.parent(child) == NullEntity);
    assert(registry.try_get<Parent>(child) == nullptr);

    assert(reparent(registry, child, first_parent));
    assert(maintenance.update());
    registry.destroy(first_parent);
    assert(maintenance.update());
    // Parent is an optional relationship, never an ownership cascade.
    assert(registry.valid(child));
    assert(registry.try_get<Parent>(child) == nullptr);
    assert(hierarchy.parent(child) == NullEntity);

    const Entity overflow_child = registry.create();
    assert(maintenance.prepare(2U));
    assert(reparent(registry, overflow_child, second_parent));
    assert(reparent(registry, overflow_child, child));
    assert(reparent(registry, overflow_child, second_parent));
    // Three notifications overflow the prepared queue, but the canonical
    // world has one relation and is recovered by a full resync.
    assert(maintenance.update());
    assert(hierarchy.synchronized());
    assert(hierarchy.parent(overflow_child) == second_parent);
    assert(!deltas.exact());

    const auto self_parent = reparent(registry, child, child);
    assert(!self_parent && self_parent.error() == EHierarchyError::SELF_PARENT);
    const auto cycle = reparent(registry, second_parent, overflow_child);
    assert(!cycle && cycle.error() == EHierarchyError::CYCLE);
}

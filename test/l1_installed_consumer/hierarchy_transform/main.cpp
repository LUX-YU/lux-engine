#include <lux/engine/simulation/SystemRegistry.hpp>
#include <lux/engine/simulation/ecs/HierarchyIndex.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/systems/TransformSystem.hpp>

int
main()
{
    using namespace lux::simulation;
    using namespace lux::simulation::ecs;

    Registry registry;
    const Entity parent = registry.create();
    const Entity child = registry.create();
    registry.emplace<Transform3D>(parent);
    registry.emplace<Transform3D>(child);
    if (!reparent(registry, child, parent))
        return 1;

    HierarchyIndex hierarchy;
    HierarchyDeltaBatch deltas;
    if (!deltas.prepare(16U))
        return 2;

    SystemRegistry systems;
    auto transform = systems.emplace<Transform3DSystem>(registry, hierarchy, deltas);
    if (!transform)
        return 3;
    auto retained = systems.retain<Transform3DSystem>(*transform);
    if (!retained || !retained->get().prepare(16U))
        return 4;
    return validSystemDescription(Transform3DSystem::Description) ? 0 : 5;
}

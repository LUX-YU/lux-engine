#include <lux/engine/simulation/ecs/HierarchyIndex.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/TransformSystem.hpp>

int main()
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

    Transform3DSystem transform(registry, hierarchy, deltas);
    if (!transform.prepare(16U))
        return 3;
    return validSimulationSystemDescription(Transform3DSystem::Description) ? 0 : 4;
}

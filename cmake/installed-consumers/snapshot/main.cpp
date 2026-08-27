#include <lux/engine/simulation/ecs/ComponentSnapshotSet.hpp>
#include <lux/engine/simulation/ecs/EcsSnapshot.hpp>

int
main()
{
    lux::simulation::ecs::ComponentSnapshotSet components;
    lux::simulation::ecs::EcsSnapshot snapshot;
    return !components.empty() || !snapshot.empty() ? 1 : 0;
}

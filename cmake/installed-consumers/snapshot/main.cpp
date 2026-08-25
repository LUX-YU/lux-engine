#include <lux/engine/ecs/ComponentSnapshotSet.hpp>
#include <lux/engine/ecs/WorldSnapshot.hpp>

int main()
{
    lux::ecs::ComponentSnapshotSet components;
    lux::ecs::WorldSnapshot snapshot;
    return !components.empty() || !snapshot.empty() ? 1 : 0;
}

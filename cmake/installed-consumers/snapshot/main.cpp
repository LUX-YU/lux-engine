#include <lux/engine/ecs/ComponentSnapshotSet.hpp>
#include <lux/engine/ecs/EcsSnapshot.hpp>

int main()
{
    lux::ecs::ComponentSnapshotSet components;
    lux::ecs::EcsSnapshot snapshot;
    return !components.empty() || !snapshot.empty() ? 1 : 0;
}

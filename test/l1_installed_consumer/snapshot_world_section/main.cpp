#include <lux/engine/ecs/ComponentSnapshotSet.hpp>
#include <lux/engine/ecs/WorldSectionImage.hpp>
#include <lux/engine/ecs/WorldSectionTransaction.hpp>
#include <lux/engine/ecs/WorldSnapshot.hpp>

#include <type_traits>

int main()
{
    lux::ecs::ComponentSnapshotSet snapshots;
    lux::ecs::WorldSnapshot snapshot;
    static_assert(!std::is_copy_constructible_v<lux::ecs::WorldSectionImage>);
    static_assert(
        !std::is_move_assignable_v<lux::ecs::WorldSectionTransaction>
    );
    return snapshots.empty() && snapshot.empty() ? 0 : 1;
}

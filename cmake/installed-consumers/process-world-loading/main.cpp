#include <lux/engine/process/world_loading/WorldPartitionLoadSender.hpp>

int main()
{
    auto sender = lux::process::world_loading::loadWorldPartition({}, {}, 1U, {});
    static_cast<void>(sender);
    return !lux::process::world_loading::WorldStorageSource{} ? 0 : 1;
}

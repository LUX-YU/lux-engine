#include <lux/engine/process/world/WorldPartitionLoadSender.hpp>

int main()
{
    auto sender = lux::process::world::loadWorldPartition({}, {}, 1U, {});
    static_cast<void>(sender);
    return !lux::process::world::WorldStorageSource{} ? 0 : 1;
}

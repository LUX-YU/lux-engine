#include <lux/engine/world/WorldPartitionData.hpp>

int main()
{
    lux::world::WorldPartitionData data;
    return data.objectCount() == 0U && !data.objectAt(0U) ? 0 : 1;
}

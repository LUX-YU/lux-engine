#include <lux/engine/partition/PartitionIndexTypeId.hpp>
#include <lux/engine/partition/PartitionOrdinal.hpp>

int main()
{
    const lux::partition::PartitionOrdinal ordinal{7U};
    const auto type = lux::partition::partitionIndexTypeId("lux.consumer.partition");
    return ordinal.value == 7U && type.valid() ? 0 : 1;
}

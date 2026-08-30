#include <lux/engine/partition/PartitionIndexTypeId.hpp>
#include <lux/engine/partition/PartitionOrdinal.hpp>

#include <cassert>
#include <type_traits>

int main()
{
    static_assert(std::is_trivially_copyable_v<lux::partition::PartitionOrdinal>);
    static_assert(sizeof(lux::partition::PartitionOrdinal) == sizeof(std::uint32_t));

    const auto type = lux::partition::partitionIndexTypeId("lux.partition.test");
    assert(type.valid());
    assert(type.name == "lux.partition.test");
    assert(type == lux::partition::partitionIndexTypeId("lux.partition.test"));
    assert(!lux::partition::PartitionIndexTypeId{}.valid());
    return 0;
}

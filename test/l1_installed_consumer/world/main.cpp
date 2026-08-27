#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <lux/engine/world/WorldPartition.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

int
main()
{
    std::array<std::uint8_t, 16> bytes{};
    bytes[15] = 1U;
    const lux::world::WorldObjectId object{uuids::uuid(bytes)};

    lux::world::WorldDescriptionBuilder builder;
    if (!builder.addObject(object))
        return 1;
    const std::array payload{std::byte{42U}};
    if (!builder.addData(object, lux::world::worldDataSchemaId("consumer.data"), 1U, payload))
    {
        return 2;
    }

    auto world = std::move(builder).build();
    if (!world || world->objectCount() != 1U)
        return 3;

    lux::world::WorldPartitionLayoutBuilder partitions(*world);
    const std::array objects{object};
    bytes[0] = 0x80U;
    bytes[15] = 2U;
    if (!partitions.addPartition(lux::world::WorldPartitionId{uuids::uuid(bytes)}, objects))
    {
        return 4;
    }
    auto layout = std::move(partitions).build();
    return layout && layout->partitionCount() == 1U ? 0 : 5;
}

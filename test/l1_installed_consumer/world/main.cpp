#include <lux/engine/world/WorldDescriptionBuilder.hpp>

#include <array>
#include <cstdint>
#include <utility>

namespace
{
    template <class Type>
    [[nodiscard]] Type id(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes[15] = tail;
        return Type{uuids::uuid(bytes)};
    }
}

int main()
{
    lux::world::WorldDescriptionBuilder builder;
    if (!builder.setIdentity(
            id<lux::world::WorldBundleId>(1U),
            id<lux::world::WorldBundleGeneration>(2U),
            "consumer"
        ))
    {
        return 1;
    }
    if (!builder.addSchema(lux::world::worldDataSchemaId("consumer.data")))
        return 2;
    if (!builder.setPartitioner({lux::world::worldPartitionerId("consumer.none"), 1U}, 0U))
        return 3;
    auto world = std::move(builder).build();
    return world && world->empty() && world->schemas().size() == 1U ? 0 : 4;
}

#include <lux/engine/world/WorldPartition.hpp>

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
    const std::array objects{id<lux::world::WorldObjectId>(1U)};
    lux::world::WorldPartitionLayoutBuilder builder(objects);
    if (!builder.addPartition(id<lux::world::WorldPartitionId>(1U), objects)) return 1;
    auto layout = std::move(builder).build();
    return layout && layout->partitionCount() == 1U ? 0 : 2;
}

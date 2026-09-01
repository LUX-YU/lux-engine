#include <lux/engine/world/WorldPartition.hpp>
#include <lux/engine/world/detail/WorldPartitionFailureInjection.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>
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
    using namespace lux::world;

    static_assert(!std::is_default_constructible_v<WorldPartitionLayout>);
    static_assert(!std::is_default_constructible_v<WorldPartitionBuildProduct>);

    const std::array objects{
        id<WorldObjectId>(1U),
        id<WorldObjectId>(2U),
        id<WorldObjectId>(3U),
        id<WorldObjectId>(4U)
    };

    {
        WorldPartitionLayoutBuilder builder(objects);
        const std::array first{objects[0], objects[1]};
        const std::array second{objects[2], objects[3]};
        assert(builder.addPartition(id<WorldPartitionId>(2U), first));
        assert(builder.addPartition(id<WorldPartitionId>(1U), second));
        auto layout = std::move(builder).build();
        assert(layout);
        assert(layout->partitionCount() == 2U);
        assert(layout->partitionAt(0U).id() == id<WorldPartitionId>(1U));
        assert(layout->partitionAt(1U).id() == id<WorldPartitionId>(2U));
        assert(layout->findPartition(id<WorldPartitionId>(2U)));
        auto product = WorldPartitionBuildProduct::build(
            {worldPartitionerId("test.layout"), 1U},
            std::move(*layout),
            {{lux::partition::partitionIndexTypeId("test.layout"), 1U, {std::byte{1U}}}}
        );
        assert(product);
        assert(product->findIndex(lux::partition::partitionIndexTypeId("test.layout")) != nullptr);
    }

    {
        WorldPartitionLayoutBuilder missing(objects);
        const std::array first{objects[0]};
        assert(missing.addPartition(id<WorldPartitionId>(1U), first));
        auto result = std::move(missing).build();
        assert(!result);
        assert(result.error().code == EWorldPartitionError::MISSING_OBJECT_ASSIGNMENT);
    }

    {
        WorldPartitionLayoutBuilder duplicate(objects);
        const std::array values{objects[0], objects[0]};
        auto result = duplicate.addPartition(id<WorldPartitionId>(1U), values);
        assert(!result);
        assert(result.error().code == EWorldPartitionError::DUPLICATE_OBJECT_ASSIGNMENT);
    }

    {
        WorldPartitionLayoutBuilder unknown(objects);
        const std::array values{id<WorldObjectId>(99U)};
        auto result = unknown.addPartition(id<WorldPartitionId>(1U), values);
        assert(!result);
        assert(result.error().code == EWorldPartitionError::UNKNOWN_OBJECT);
    }

    {
        const std::array duplicate_universe{objects[0], objects[0]};
        WorldPartitionLayoutBuilder invalid(duplicate_universe);
        auto result = std::move(invalid).build();
        assert(!result);
        assert(result.error().code == EWorldPartitionError::DUPLICATE_OBJECT_ID);
    }

    {
        WorldPartitionLayoutBuilder allocation(objects);
        const std::array values{objects[0], objects[1], objects[2], objects[3]};
        detail::failNextWorldPartitionOperationForTest(
            detail::EWorldPartitionFailurePoint::MUTATION_ALLOCATION
        );
        assert(!allocation.addPartition(id<WorldPartitionId>(1U), values));
        assert(allocation.addPartition(id<WorldPartitionId>(1U), values));
        detail::failNextWorldPartitionOperationForTest(detail::EWorldPartitionFailurePoint::BUILD_ALLOCATION);
        assert(!std::move(allocation).build());
        assert(std::move(allocation).build());
    }

    return 0;
}

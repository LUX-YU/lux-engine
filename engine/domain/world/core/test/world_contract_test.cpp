#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <lux/engine/world/detail/WorldFailureInjection.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
    template <class Type>
    [[nodiscard]] Type id(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes[15] = tail;
        return Type{uuids::uuid(bytes)};
    }

    [[nodiscard]] lux::world::WorldDescription makeDescription(std::uint32_t partition_count)
    {
        using namespace lux::world;

        WorldDescriptionBuilder builder;
        assert(builder.setIdentity(id<WorldBundleId>(1U), id<WorldBundleGeneration>(2U), "test-world"));
        assert(builder.addSchema(worldDataSchemaId("test.position")));
        assert(builder.addSchema(worldDataSchemaId("test.renderable")));
        assert(builder.setPartitioner({worldPartitionerId("test.grid"), 1U}, partition_count));

        if (partition_count != 0U)
        {
            assert(builder.addStorageVolume({"test-world.wvol0", 1U, 2U, 4096U}));
            assert(builder.addPartitionTablePage({WorldPartitionOrdinal{0U}, partition_count, {0U, 0U}}));
            assert(builder.addPartitionIndex({worldPartitionIndexTypeId("test.grid"), 1U, {0U, 1U}}));
        }

        auto result = std::move(builder).build();
        assert(result);
        return std::move(*result);
    }
}

int main()
{
    using namespace lux::world;

    static_assert(!std::is_default_constructible_v<WorldPartitionLayout>);
    static_assert(!std::is_default_constructible_v<WorldPartitionBuildProduct>);

    {
        WorldDescriptionBuilder builder;
        assert(!builder.setIdentity({}, id<WorldBundleGeneration>(2U), "invalid"));
        assert(!builder.setIdentity(id<WorldBundleId>(1U), {}, "invalid"));
        assert(!builder.setIdentity(id<WorldBundleId>(1U), id<WorldBundleGeneration>(2U), ""));
        assert(builder.setIdentity(id<WorldBundleId>(1U), id<WorldBundleGeneration>(2U), "valid"));
        assert(!builder.addSchema({}));
        assert(builder.addSchema(worldDataSchemaId("test.schema")));
        assert(!builder.addSchema(worldDataSchemaId("test.schema")));
        assert(!builder.setPartitioner({}, 0U));
        assert(builder.setPartitioner({worldPartitionerId("test.none"), 1U}, 0U));
        assert(!builder.addStorageVolume({"../escape.wvol", 1U, 1U, 64U}));
        assert(!builder.addStorageVolume({"C:/escape.wvol", 1U, 1U, 64U}));
        assert(!builder.addStorageVolume({"nested\\escape.wvol", 1U, 1U, 64U}));
        auto empty = std::move(builder).build();
        assert(empty);
        assert(empty->empty());
        assert(empty->partitionTable().pages().empty());
    }

    WorldDescription world = makeDescription(4U);
    assert(!world.empty());
    assert(world.bundleId() == id<WorldBundleId>(1U));
    assert(world.generation() == id<WorldBundleGeneration>(2U));
    assert(world.name() == "test-world");
    assert(world.schemas().size() == 2U);
    assert(world.schemas()[0].name < world.schemas()[1].name);
    assert(world.partitionCount() == 4U);
    assert(world.storageVolumes().size() == 1U);
    assert(world.partitionTable().pages().size() == 1U);
    assert(world.partitionTable().findPage(WorldPartitionOrdinal{0U}) != nullptr);
    assert(world.partitionTable().findPage(WorldPartitionOrdinal{3U}) != nullptr);
    assert(world.partitionTable().findPage(WorldPartitionOrdinal{4U}) == nullptr);
    assert(world.partitionIndexes().size() == 1U);

    {
        WorldDescriptionBuilder gap;
        assert(gap.setIdentity(id<WorldBundleId>(3U), id<WorldBundleGeneration>(4U), "gap"));
        assert(gap.setPartitioner({worldPartitionerId("test.gap"), 1U}, 4U));
        assert(gap.addStorageVolume({"gap.wvol0", 1U, 1U, 64U}));
        assert(gap.addPartitionTablePage({WorldPartitionOrdinal{1U}, 3U, {0U, 0U}}));
        auto result = std::move(gap).build();
        assert(!result);
        assert(result.error().code == EWorldDescriptionError::PARTITION_PAGE_GAP);
    }

    {
        WorldDescriptionBuilder overlap;
        assert(overlap.setIdentity(id<WorldBundleId>(5U), id<WorldBundleGeneration>(6U), "overlap"));
        assert(overlap.setPartitioner({worldPartitionerId("test.overlap"), 1U}, 4U));
        assert(overlap.addStorageVolume({"overlap.wvol0", 1U, 2U, 64U}));
        assert(overlap.addPartitionTablePage({WorldPartitionOrdinal{0U}, 3U, {0U, 0U}}));
        assert(overlap.addPartitionTablePage({WorldPartitionOrdinal{2U}, 2U, {0U, 1U}}));
        auto result = std::move(overlap).build();
        assert(!result);
        assert(result.error().code == EWorldDescriptionError::PARTITION_PAGE_OVERLAP);
    }

    {
        WorldDescriptionBuilder invalid_reference;
        assert(invalid_reference.setIdentity(
            id<WorldBundleId>(7U),
            id<WorldBundleGeneration>(8U),
            "invalid-reference"
        ));
        assert(invalid_reference.setPartitioner({worldPartitionerId("test.invalid-reference"), 1U}, 1U));
        assert(invalid_reference.addStorageVolume({"invalid-reference.wvol0", 1U, 1U, 64U}));
        assert(invalid_reference.addPartitionTablePage({WorldPartitionOrdinal{0U}, 1U, {0U, 1U}}));
        auto result = std::move(invalid_reference).build();
        assert(!result);
        assert(result.error().code == EWorldDescriptionError::INVALID_CHUNK_REFERENCE);
    }

    {
        WorldDescriptionBuilder allocation;
        detail::failNextWorldOperationForTest(detail::EWorldFailurePoint::DESCRIPTION_MUTATION_ALLOCATION);
        assert(!allocation.setIdentity(
            id<WorldBundleId>(9U),
            id<WorldBundleGeneration>(10U),
            "allocation"
        ));
        assert(allocation.setIdentity(
            id<WorldBundleId>(9U),
            id<WorldBundleGeneration>(10U),
            "allocation"
        ));
        assert(allocation.setPartitioner({worldPartitionerId("test.allocation"), 1U}, 0U));
        detail::failNextWorldOperationForTest(detail::EWorldFailurePoint::DESCRIPTION_BUILD_ALLOCATION);
        assert(!std::move(allocation).build());
        auto recovered = std::move(allocation).build();
        assert(recovered);
    }

    const WorldDescription million = makeDescription(1'000'000U);
    assert(million.partitionCount() == 1'000'000U);
    assert(million.partitionTable().pages().size() == 1U);
    assert(million.retainedBytes() < 4096U);
    assert(million.retainedBytes() <= world.retainedBytes() + 256U);

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
            {{worldPartitionIndexTypeId("test.layout"), 1U, {std::byte{1U}}}}
        );
        assert(product);
        assert(product->findIndex(worldPartitionIndexTypeId("test.layout")) != nullptr);
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
        detail::failNextWorldOperationForTest(detail::EWorldFailurePoint::PARTITION_MUTATION_ALLOCATION);
        assert(!allocation.addPartition(id<WorldPartitionId>(1U), values));
        assert(allocation.addPartition(id<WorldPartitionId>(1U), values));
        detail::failNextWorldOperationForTest(detail::EWorldFailurePoint::PARTITION_BUILD_ALLOCATION);
        assert(!std::move(allocation).build());
        assert(std::move(allocation).build());
    }

    return 0;
}

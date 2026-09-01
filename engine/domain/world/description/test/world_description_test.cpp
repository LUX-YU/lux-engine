#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <lux/engine/world/detail/WorldDescriptionFailureInjection.hpp>

#include <array>
#include <cassert>
#include <cstddef>
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
            assert(builder.addPartitionTablePage({lux::partition::PartitionOrdinal{0U}, partition_count, {0U, 0U}}));
            assert(builder.addPartitionIndex({lux::partition::partitionIndexTypeId("test.grid"), 1U, {0U, 1U}}));
        }

        auto result = std::move(builder).build();
        assert(result);
        return std::move(*result);
    }
}

int main()
{
    using namespace lux::world;

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
    assert(world.partitionTable().findPage(lux::partition::PartitionOrdinal{0U}) != nullptr);
    assert(world.partitionTable().findPage(lux::partition::PartitionOrdinal{3U}) != nullptr);
    assert(world.partitionTable().findPage(lux::partition::PartitionOrdinal{4U}) == nullptr);
    assert(world.partitionIndexes().size() == 1U);

    {
        WorldDescriptionBuilder gap;
        assert(gap.setIdentity(id<WorldBundleId>(3U), id<WorldBundleGeneration>(4U), "gap"));
        assert(gap.setPartitioner({worldPartitionerId("test.gap"), 1U}, 4U));
        assert(gap.addStorageVolume({"gap.wvol0", 1U, 1U, 64U}));
        assert(gap.addPartitionTablePage({lux::partition::PartitionOrdinal{1U}, 3U, {0U, 0U}}));
        auto result = std::move(gap).build();
        assert(!result);
        assert(result.error().code == EWorldDescriptionError::PARTITION_PAGE_GAP);
    }

    {
        WorldDescriptionBuilder overlap;
        assert(overlap.setIdentity(id<WorldBundleId>(5U), id<WorldBundleGeneration>(6U), "overlap"));
        assert(overlap.setPartitioner({worldPartitionerId("test.overlap"), 1U}, 4U));
        assert(overlap.addStorageVolume({"overlap.wvol0", 1U, 2U, 64U}));
        assert(overlap.addPartitionTablePage({lux::partition::PartitionOrdinal{0U}, 3U, {0U, 0U}}));
        assert(overlap.addPartitionTablePage({lux::partition::PartitionOrdinal{2U}, 2U, {0U, 1U}}));
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
        assert(invalid_reference.addPartitionTablePage({lux::partition::PartitionOrdinal{0U}, 1U, {0U, 1U}}));
        auto result = std::move(invalid_reference).build();
        assert(!result);
        assert(result.error().code == EWorldDescriptionError::INVALID_CHUNK_REFERENCE);
    }

    {
        WorldDescriptionBuilder allocation;
        detail::failNextWorldDescriptionOperationForTest(
            detail::EWorldDescriptionFailurePoint::MUTATION_ALLOCATION
        );
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
        detail::failNextWorldDescriptionOperationForTest(detail::EWorldDescriptionFailurePoint::BUILD_ALLOCATION);
        assert(!std::move(allocation).build());
        auto recovered = std::move(allocation).build();
        assert(recovered);
    }

    const WorldDescription million = makeDescription(1'000'000U);
    assert(million.partitionCount() == 1'000'000U);
    assert(million.partitionTable().pages().size() == 1U);
    assert(million.retainedBytes() < 4096U);
    assert(million.retainedBytes() <= world.retainedBytes() + 256U);

    return 0;
}

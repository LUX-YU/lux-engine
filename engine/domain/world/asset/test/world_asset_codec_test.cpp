#include <lux/engine/world/WorldAssetCodec.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
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

    [[nodiscard]] lux::world::WorldDescription makeWorld()
    {
        using namespace lux::world;

        WorldDescriptionBuilder builder;
        assert(builder.setIdentity(id<WorldBundleId>(1U), id<WorldBundleGeneration>(2U), "world"));
        assert(builder.addSchema(worldDataSchemaId("test.aa")));
        assert(builder.addSchema(worldDataSchemaId("test.bb")));
        assert(builder.setPartitioner({worldPartitionerId("test.grid"), 1U}, 4U));
        assert(builder.addStorageVolume({"world.wvol0", 1U, 3U, 4096U}));
        assert(builder.addPartitionTablePage({WorldPartitionOrdinal{0U}, 2U, {0U, 0U}}));
        assert(builder.addPartitionTablePage({WorldPartitionOrdinal{2U}, 2U, {0U, 1U}}));
        assert(builder.addPartitionIndex({worldPartitionIndexTypeId("test.grid"), 1U, {0U, 2U}}));
        auto result = std::move(builder).build();
        assert(result);
        return std::move(*result);
    }
}

int main()
{
    using namespace lux::asset;
    using namespace lux::world;

    auto code_lifetime = std::make_shared<int>(42);
    auto descriptor = worldAssetCodecDescriptor(code_lifetime);
    assert(descriptor.type == AssetTypeId::fromName(WorldAssetCanonicalName));
    assert(descriptor.canonical_name == WorldAssetCanonicalName);
    assert(descriptor.primary_magic == WorldAssetPrimaryMagic);
    assert(descriptor.legacy_magic == 0U);
    assert(descriptor.cpp_payload_type == lux::cxx::typeToken<WorldDescription>());
    assert(code_lifetime.use_count() == 2U);

    auto world = makeWorld();
    const AssetEncodeContext generous_encode{AssetCodecLimits{0U, 0U, std::numeric_limits<std::size_t>::max()}};
    auto encoded = descriptor.encode(&world, generous_encode);
    assert(encoded);
    assert(!descriptor.encode(&world, AssetEncodeContext{AssetCodecLimits{0U, 0U, encoded->size() - 1U}}));

    const AssetDecodeContext generous_decode{
        AssetCodecLimits{encoded->size(), std::numeric_limits<std::size_t>::max(), 0U}
    };
    auto decoded = descriptor.decode(*encoded, generous_decode);
    assert(decoded);
    auto decoded_world = std::static_pointer_cast<const WorldDescription>(decoded->payload);
    assert(decoded_world);
    assert(decoded_world->bundleId() == world.bundleId());
    assert(decoded_world->generation() == world.generation());
    assert(decoded_world->name() == world.name());
    assert(decoded_world->schemas().size() == world.schemas().size());
    for (std::size_t index{}; index < world.schemas().size(); ++index)
        assert(decoded_world->schemas()[index] == world.schemas()[index]);
    assert(decoded_world->partitioner().id == world.partitioner().id);
    assert(decoded_world->partitioner().version == world.partitioner().version);
    assert(decoded_world->partitionCount() == world.partitionCount());
    assert(decoded_world->storageVolumes().size() == world.storageVolumes().size());
    assert(decoded_world->partitionTable().pages().size() == world.partitionTable().pages().size());
    assert(decoded_world->partitionIndexes().size() == world.partitionIndexes().size());
    assert(decoded->decoded_byte_count == decoded_world->retainedBytes());

    assert(!descriptor.decode(
        *encoded,
        AssetDecodeContext{AssetCodecLimits{encoded->size() - 1U, std::numeric_limits<std::size_t>::max(), 0U}}
    ));
    assert(!descriptor.decode(*encoded, AssetDecodeContext{AssetCodecLimits{encoded->size(), 1U, 0U}}));

    auto trailing = *encoded;
    trailing.push_back(std::byte{});
    assert(!descriptor.decode(
        trailing,
        AssetDecodeContext{AssetCodecLimits{trailing.size(), std::numeric_limits<std::size_t>::max(), 0U}}
    ));

    auto corrupt_magic = *encoded;
    corrupt_magic[0] ^= std::byte{0x01U};
    assert(!descriptor.decode(corrupt_magic, generous_decode));

    auto corrupt_version = *encoded;
    corrupt_version[4] ^= std::byte{0x01U};
    assert(!descriptor.decode(corrupt_version, generous_decode));

    auto truncated = *encoded;
    truncated.pop_back();
    assert(!descriptor.decode(truncated, generous_decode));

    // Header 40 bytes + name record 9 bytes + schema count 4 bytes.
    auto noncanonical = *encoded;
    constexpr std::size_t kSchemaOffset = 53U;
    constexpr std::size_t kSchemaRecordBytes = 8U + 4U + 7U;
    for (std::size_t index{}; index < kSchemaRecordBytes; ++index)
    {
        std::swap(
            noncanonical[kSchemaOffset + index],
            noncanonical[kSchemaOffset + kSchemaRecordBytes + index]
        );
    }
    assert(!descriptor.decode(noncanonical, generous_decode));

    return 0;
}

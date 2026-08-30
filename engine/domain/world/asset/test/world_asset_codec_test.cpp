#include <lux/cxx/algorithm/sha256.hpp>

#include <lux/engine/resource/asset/CookedAssetImage.hpp>
#include <lux/engine/world/WorldAssetCodec.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace
{
    template <class Type>
    [[nodiscard]] Type id(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16U> bytes{};
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
        assert(builder.addPartitionTablePage({lux::partition::PartitionOrdinal{0U}, 2U, {0U, 0U}}));
        assert(builder.addPartitionTablePage({lux::partition::PartitionOrdinal{2U}, 2U, {0U, 1U}}));
        assert(builder.addPartitionIndex({lux::partition::partitionIndexTypeId("test.grid"), 1U, {0U, 2U}}));
        auto result = std::move(builder).build();
        assert(result);
        return std::move(*result);
    }

    [[nodiscard]] std::string sha256(std::span<const std::byte> bytes)
    {
        const auto digest = lux::cxx::algorithm::Sha256::hash(bytes);
        std::array<char, lux::cxx::algorithm::Sha256Digest::hex_size> text{};
        digest.formatHex(text);
        return {text.data(), text.size()};
    }
} // namespace

int main()
{
    using namespace lux::asset;
    using namespace lux::world;
    constexpr AssetEncodeLimits encode_limits{1024U * 1024U};
    constexpr AssetDecodeLimits decode_limits{1024U * 1024U, 1024U * 1024U, 4U};

    auto world = std::make_shared<const WorldDescription>(makeWorld());
    auto asset = WorldAsset::create(
        AssetInfo{id<AssetId>(9U), WorldAsset::asset_type, 17U},
        world
    );
    assert(asset);
    auto encoded = TAssetSerDeser<WorldAsset>::encode(**asset, encode_limits);
    assert(encoded);
    const auto outer = inspectCookedAssetImage(
        (*asset)->id(),
        lux::cxx::SharedBytes<>::copyOf(*encoded),
        decode_limits
    );
    assert(outer);
    assert(outer->information().empty());
    assert(outer->data().size() == 228U);
    assert(sha256(outer->data().view()) ==
        "dea3eca1af27b347bc525dbdb328437dad8c22cbb599bace44c7b10eb8064993");

    const auto decoded = TAssetSerDeser<WorldAsset>::decode(
        (*asset)->id(),
        lux::cxx::SharedBytes<>::copyOf(*encoded),
        decode_limits
    );
    assert(decoded);
    const auto& value = (*decoded)->data();
    assert(value.bundleId() == world->bundleId());
    assert(value.generation() == world->generation());
    assert(value.name() == world->name());
    assert(value.schemas().size() == world->schemas().size());
    assert(value.partitionCount() == world->partitionCount());
    assert(value.storageVolumes().size() == world->storageVolumes().size());
    assert(value.partitionTable().pages().size() == world->partitionTable().pages().size());
    assert(value.partitionIndexes().size() == world->partitionIndexes().size());

    const auto limited = TAssetSerDeser<WorldAsset>::decode(
        (*asset)->id(),
        lux::cxx::SharedBytes<>::copyOf(*encoded),
        AssetDecodeLimits{encoded->size(), 1U, 4U}
    );
    assert(!limited && limited.error().code == EAssetDecodeError::LIMIT_EXCEEDED);

    auto trailing = *encoded;
    trailing.push_back(std::byte{});
    assert(!TAssetSerDeser<WorldAsset>::decode(
        (*asset)->id(),
        lux::cxx::SharedBytes<>::copyOf(trailing),
        AssetDecodeLimits{trailing.size(), trailing.size(), 4U}
    ));

    auto corrupt_magic = *encoded;
    corrupt_magic[400U] ^= std::byte{0x01U};
    assert(!TAssetSerDeser<WorldAsset>::decode(
        (*asset)->id(),
        lux::cxx::SharedBytes<>::copyOf(corrupt_magic),
        decode_limits
    ));

    auto noncanonical = *encoded;
    constexpr std::size_t kSchemaOffset = 400U + 53U;
    constexpr std::size_t kSchemaRecordBytes = 8U + 4U + 7U;
    for (std::size_t index{}; index < kSchemaRecordBytes; ++index)
    {
        std::swap(
            noncanonical[kSchemaOffset + index],
            noncanonical[kSchemaOffset + kSchemaRecordBytes + index]
        );
    }
    assert(!TAssetSerDeser<WorldAsset>::decode(
        (*asset)->id(),
        lux::cxx::SharedBytes<>::copyOf(noncanonical),
        decode_limits
    ));

    assert(!TAssetSerDeser<WorldAsset>::decode(
        id<AssetId>(10U),
        lux::cxx::SharedBytes<>::copyOf(*encoded),
        decode_limits
    ));
    return 0;
}

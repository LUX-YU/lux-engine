#include <lux/engine/world/WorldAssetCodec.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace
{
    [[nodiscard]] lux::world::WorldObjectId objectId(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes[15] = tail;
        return lux::world::WorldObjectId{uuids::uuid(bytes)};
    }

    [[nodiscard]] lux::world::WorldDescription makeWorld()
    {
        using namespace lux::world;
        WorldDescriptionBuilder builder;
        const auto schema_a = worldDataSchemaId("test.aa");
        const auto schema_b = worldDataSchemaId("test.bb");
        const auto first = objectId(1U);
        const auto second = objectId(2U);
        const std::array payload_a{std::byte{1U}, std::byte{2U}};
        const std::array payload_b{std::byte{3U}};
        assert(builder.addObject(second));
        assert(builder.addObject(first));
        assert(builder.addData(first, schema_b, 2U, payload_b));
        assert(builder.addData(first, schema_a, 1U, payload_a));
        auto result = std::move(builder).build();
        assert(result);
        return std::move(*result);
    }
}

int
main()
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
        AssetCodecLimits{encoded->size(), std::numeric_limits<std::size_t>::max(), 0U}};
    auto decoded = descriptor.decode(*encoded, generous_decode);
    assert(decoded);
    auto decoded_world = std::static_pointer_cast<const WorldDescription>(decoded->payload);
    assert(decoded_world);
    assert(decoded_world->objectCount() == world.objectCount());
    assert(decoded_world->dataCount() == world.dataCount());
    assert(decoded->decoded_byte_count == decoded_world->retainedBytes());
    assert(!descriptor.decode(
        *encoded,
        AssetDecodeContext{AssetCodecLimits{encoded->size() - 1U, std::numeric_limits<std::size_t>::max(), 0U}}));
    assert(!descriptor.decode(*encoded, AssetDecodeContext{AssetCodecLimits{encoded->size(), 1U, 0U}}));

    auto trailing = *encoded;
    trailing.push_back(std::byte{});
    assert(!descriptor.decode(
        trailing,
        AssetDecodeContext{AssetCodecLimits{trailing.size(), std::numeric_limits<std::size_t>::max(), 0U}}));
    auto corrupt_magic = *encoded;
    corrupt_magic[0] ^= std::byte{0x01U};
    assert(!descriptor.decode(corrupt_magic, generous_decode));
    auto truncated = *encoded;
    truncated.pop_back();
    assert(!descriptor.decode(truncated, generous_decode));

    // Both schema names are seven bytes, so their complete wire records have
    // equal length. Swapping them produces a structurally valid but
    // non-canonical schema table that decode must reject.
    auto noncanonical = *encoded;
    constexpr std::size_t header_bytes = 48U;
    constexpr std::size_t schema_record_bytes = 16U + 7U;
    for (std::size_t index{}; index < schema_record_bytes; ++index)
    {
        std::swap(noncanonical[header_bytes + index], noncanonical[header_bytes + schema_record_bytes + index]);
    }
    assert(!descriptor.decode(noncanonical, generous_decode));

    return 0;
}

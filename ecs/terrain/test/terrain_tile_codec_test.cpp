#include <lux/engine/ecs/terrain/TerrainTileCodec.hpp>
#include <lux/cxx/algorithm/sha256.hpp>

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace
{
    using namespace lux::terrain;

    [[nodiscard]] TerrainTileBlobV1 makeTile()
    {
        TerrainTileBlobV1 result;
        result.height_min = -32.0f;
        result.height_max = 96.0f;
        result.sample_spacing = 0.5f;
        result.weight_layer_count = 3u;
        result.heights.resize(kTerrainTileSampleCount);
        for (std::size_t index = 0u; index < result.heights.size(); ++index)
        {
            result.heights[index] = static_cast<std::uint16_t>(
                index % 1024u);
        }
        for (std::size_t plane = 0u; plane < 2u; ++plane)
        {
            result.weight_planes[plane].resize(
                kTerrainTileWeightPlaneBytes);
            for (std::size_t index = 0u;
                 index < result.weight_planes[plane].size();
                 ++index)
            {
                result.weight_planes[plane][index] =
                    static_cast<std::uint8_t>((index + plane * 17u) & 0xffu);
            }
        }
        result.holes.assign(kTerrainTileHoleBytes, 0u);
        result.holes[0] = 1u;
        result.min_max_pairs.resize(
            static_cast<std::size_t>(kTerrainTileMinMaxNodeCount) * 2u);
        for (std::size_t index = 0u;
             index < result.min_max_pairs.size();
             index += 2u)
        {
            result.min_max_pairs[index] = 0u;
            result.min_max_pairs[index + 1u] = 1023u;
        }
        result.parent_fallback_heights.assign(
            kTerrainTileFallbackSampleCount,
            512u);
        return result;
    }

    void writeU16(
        std::vector<std::byte>& bytes,
        std::size_t offset,
        std::uint16_t value)
    {
        assert(offset + 2u <= bytes.size());
        bytes[offset] = static_cast<std::byte>(value & 0xffu);
        bytes[offset + 1u] = static_cast<std::byte>((value >> 8u) & 0xffu);
    }

    void writeU32(
        std::vector<std::byte>& bytes,
        std::size_t offset,
        std::uint32_t value)
    {
        assert(offset + 4u <= bytes.size());
        for (std::size_t index = 0u; index < 4u; ++index)
        {
            bytes[offset + index] = static_cast<std::byte>(
                (value >> (index * 8u)) & 0xffu);
        }
    }

    void expectError(
        const std::vector<std::byte>& bytes,
        ETerrainTileCodecError error)
    {
        const auto decoded = decodeTerrainTileBlob(bytes);
        assert(!decoded);
        assert(decoded.error().error == error);
    }
}

int main()
{
    static_assert(kTerrainTileSchemaVersion == 1u);
    assert(kTerrainTileContentTypeName == "lux.terrain.tile");

    const auto source = makeTile();
    const auto encoded = encodeTerrainTileBlob(source);
    assert(encoded);
    assert(encoded->size() == 1051597u);
    assert(lux::cxx::algorithm::toHex(
        lux::cxx::algorithm::Sha256::hash(*encoded)) ==
        "f1edc77ca1ae5f531f87c0895ef492daa7615621af3e24a1d279dee55cae5e84");
    const auto decoded = decodeTerrainTileBlob(*encoded);
    assert(decoded);
    assert(*decoded == source);
    const auto encoded_again = encodeTerrainTileBlob(*decoded);
    assert(encoded_again);
    assert(*encoded_again == *encoded);

    auto malformed = *encoded;
    malformed.pop_back();
    expectError(malformed, ETerrainTileCodecError::TRUNCATED);

    malformed = *encoded;
    malformed.push_back(std::byte{0u});
    expectError(malformed, ETerrainTileCodecError::TRAILING_BYTES);

    malformed = *encoded;
    writeU32(malformed, 0u, 0u);
    expectError(malformed, ETerrainTileCodecError::BAD_MAGIC);

    malformed = *encoded;
    writeU32(malformed, 4u, kTerrainTileSchemaVersion + 1u);
    expectError(malformed, ETerrainTileCodecError::UNSUPPORTED_VERSION);

    malformed = *encoded;
    malformed[21u] = std::byte{1u};
    expectError(malformed, ETerrainTileCodecError::INVALID_LAYOUT);

    malformed = *encoded;
    writeU32(malformed, 24u, 0xffffffffu);
    expectError(malformed, ETerrainTileCodecError::INVALID_LAYOUT);

    malformed = *encoded;
    writeU32(
        malformed,
        8u,
        std::bit_cast<std::uint32_t>(
            std::numeric_limits<float>::quiet_NaN()));
    expectError(malformed, ETerrainTileCodecError::INVALID_VALUE);

    malformed = *encoded;
    malformed[20u] = static_cast<std::byte>(
        kTerrainTileMaximumWeightLayers + 1u);
    expectError(malformed, ETerrainTileCodecError::INVALID_VALUE);

    constexpr std::size_t kHeaderBytes = 44u;
    const std::size_t holes_offset = kHeaderBytes +
        static_cast<std::size_t>(kTerrainTileSampleCount) * 2u +
        static_cast<std::size_t>(kTerrainTileWeightPlaneBytes) * 2u;
    malformed = *encoded;
    malformed[holes_offset + kTerrainTileHoleBytes - 1u] =
        std::byte{0x80u};
    expectError(malformed, ETerrainTileCodecError::INVALID_VALUE);

    const std::size_t min_max_offset =
        holes_offset + kTerrainTileHoleBytes;
    malformed = *encoded;
    writeU16(malformed, min_max_offset, 10u);
    writeU16(malformed, min_max_offset + 2u, 5u);
    expectError(malformed, ETerrainTileCodecError::INVALID_VALUE);

    auto invalid_source = source;
    invalid_source.heights.pop_back();
    const auto invalid_layout = encodeTerrainTileBlob(invalid_source);
    assert(!invalid_layout);
    assert(invalid_layout.error().error ==
        ETerrainTileCodecError::INVALID_LAYOUT);

    invalid_source = source;
    invalid_source.min_max_pairs[
        invalid_source.min_max_pairs.size() - 1u] = 1000u;
    const auto invalid_summary = encodeTerrainTileBlob(invalid_source);
    assert(!invalid_summary);
    assert(invalid_summary.error().error ==
        ETerrainTileCodecError::INVALID_VALUE);
    return 0;
}

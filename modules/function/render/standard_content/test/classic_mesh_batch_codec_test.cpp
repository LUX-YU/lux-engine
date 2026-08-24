#include <lux/engine/function/render/standard/content/ClassicMeshBatch.hpp>
#include <lux/cxx/algorithm/sha256.hpp>

#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace
{
    using namespace lux::classic_mesh;

    [[nodiscard]] lux::asset::AssetId assetId(std::uint8_t seed)
    {
        std::array<std::uint8_t, 16u> bytes{};
        for (std::size_t index = 0u; index < bytes.size(); ++index)
            bytes[index] = static_cast<std::uint8_t>(seed + index);
        return lux::asset::AssetId{bytes};
    }

    [[nodiscard]] ClassicMeshBatchBlobV1 makeBlob()
    {
        ClassicMeshBatchInstanceV1 first;
        first.translation = {1.0f, 2.0f, 3.0f};
        first.mesh_asset = assetId(1u);
        first.material_asset = assetId(33u);
        first.stable_pick_id = 11u;
        first.rgba8 = 0xff3366ffu;

        ClassicMeshBatchInstanceV1 second;
        second.translation = {-4.0f, 0.5f, 8.0f};
        second.rotation = {0.0f, 0.0f, 1.0f, 0.0f};
        second.scale = {-1.0f, 2.0f, 0.5f};
        second.mesh_asset = assetId(65u);
        second.stable_pick_id = 12u;
        second.rgba8 = 0x44cc88ffu;
        second.flags = static_cast<std::uint32_t>(
            EClassicMeshInstanceFlag::VISIBLE);
        return {{first, second}};
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
        EClassicMeshBatchCodecError error)
    {
        const auto decoded = decodeClassicMeshBatchBlob(bytes);
        assert(!decoded);
        assert(decoded.error().error == error);
    }
}

int main()
{
    static_assert(kClassicMeshBatchSchemaVersion == 1u);
    assert(kClassicMeshBatchContentTypeName ==
        "lux.render.geometry.classic_mesh.batch");

    const auto source = makeBlob();
    const auto encoded = encodeClassicMeshBatchBlob(source);
    assert(encoded);
    assert(encoded->size() == 188u);
    assert(lux::cxx::algorithm::toHex(
        lux::cxx::algorithm::Sha256::hash(*encoded)) ==
        "7f8b4a4d5506fba18b2587725480cde3dbdd54114b94ef6e6e2bd5ead96a99ec");
    const auto decoded = decodeClassicMeshBatchBlob(*encoded);
    assert(decoded);
    assert(*decoded == source);
    const auto encoded_again = encodeClassicMeshBatchBlob(*decoded);
    assert(encoded_again);
    assert(*encoded_again == *encoded);

    auto malformed = *encoded;
    malformed.pop_back();
    expectError(malformed, EClassicMeshBatchCodecError::TRUNCATED);

    malformed = *encoded;
    malformed.push_back(std::byte{0u});
    expectError(malformed, EClassicMeshBatchCodecError::TRAILING_BYTES);

    malformed = *encoded;
    writeU32(malformed, 0u, 0u);
    expectError(malformed, EClassicMeshBatchCodecError::BAD_MAGIC);

    malformed = *encoded;
    writeU32(malformed, 4u, kClassicMeshBatchSchemaVersion + 1u);
    expectError(
        malformed,
        EClassicMeshBatchCodecError::UNSUPPORTED_VERSION);

    malformed = *encoded;
    writeU32(malformed, 8u, 0u);
    expectError(malformed, EClassicMeshBatchCodecError::INVALID_INSTANCE);

    malformed = *encoded;
    writeU32(malformed, 8u, 0xffffffffu);
    expectError(malformed, EClassicMeshBatchCodecError::LIMIT_EXCEEDED);

    // First row starts at byte 12.  A non-finite translation must be rejected
    // after structural decoding rather than reaching a render extractor.
    malformed = *encoded;
    writeU32(
        malformed,
        12u,
        std::bit_cast<std::uint32_t>(
            std::numeric_limits<float>::quiet_NaN()));
    expectError(malformed, EClassicMeshBatchCodecError::INVALID_INSTANCE);

    // Flags are the final u32 of each fixed 88-byte row.
    malformed = *encoded;
    writeU32(malformed, 12u + 84u, 0x80000000u);
    expectError(malformed, EClassicMeshBatchCodecError::INVALID_INSTANCE);

    // Bit 3 used to advertise an unimplemented two-sided-shadow promise.
    // Wire v1 now rejects it as unknown instead of silently dropping it.
    malformed = *encoded;
    writeU32(malformed, 12u + 84u, 1u << 3u);
    expectError(malformed, EClassicMeshBatchCodecError::INVALID_INSTANCE);

    // A nil mesh ID is never interpreted as an implicit runtime fallback.
    malformed = *encoded;
    for (std::size_t index = 0u; index < 16u; ++index)
        malformed[12u + 40u + index] = std::byte{0u};
    expectError(malformed, EClassicMeshBatchCodecError::INVALID_INSTANCE);

    auto invalid_source = source;
    invalid_source.instances[0].rotation = {0.0f, 0.0f, 0.0f, 0.0f};
    assert(!encodeClassicMeshBatchBlob(invalid_source));

    const ClassicMeshBatchCodecLimits tiny_limit{
        .maximum_instances = 1u,
        .maximum_encoded_bytes = 512u};
    const auto limited = decodeClassicMeshBatchBlob(*encoded, tiny_limit);
    assert(!limited);
    assert(limited.error().error ==
        EClassicMeshBatchCodecError::LIMIT_EXCEEDED);
    return 0;
}

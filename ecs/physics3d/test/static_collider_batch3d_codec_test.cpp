#include <lux/engine/ecs/physics3d/StaticColliderBatch3DCodec.hpp>
#include <lux/cxx/algorithm/sha256.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace
{
    using namespace lux::physics3d;

    [[nodiscard]] StaticColliderBatch3DBlobV1 makeBatch()
    {
        StaticColliderBatch3DBlobV1 result;
        auto& heightfield = result.heightfields.emplace_back();
        heightfield.local_origin = {0.25, -2.0, 0.5};
        heightfield.sample_edge = 3u;
        heightfield.sample_spacing = 0.5f;
        heightfield.height_min = -4.0f;
        heightfield.height_max = 8.0f;
        heightfield.samples = {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
        return result;
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
        EStaticColliderBatch3DCodecError error)
    {
        const auto decoded = decodeStaticColliderBatch3DBlob(bytes);
        assert(!decoded);
        assert(decoded.error().error == error);
    }
} // namespace

int main()
{
    const auto source = makeBatch();
    const auto encoded = encodeStaticColliderBatch3DBlob(source);
    assert(encoded);
    assert(encoded->size() == 78u);
    assert(lux::cxx::algorithm::toHex(
        lux::cxx::algorithm::Sha256::hash(*encoded)) ==
        "9fe8d8351e968bef204fb42f676d7f8a67cccfd2a2ccf0d6412e2b66846cbff8");
    const auto decoded = decodeStaticColliderBatch3DBlob(*encoded);
    assert(decoded);
    assert(*decoded == source);
    const auto encoded_again = encodeStaticColliderBatch3DBlob(*decoded);
    assert(encoded_again);
    assert(*encoded_again == *encoded);

    auto malformed = *encoded;
    malformed.pop_back();
    expectError(malformed, EStaticColliderBatch3DCodecError::TRUNCATED);

    malformed = *encoded;
    malformed.push_back(std::byte{0u});
    expectError(malformed, EStaticColliderBatch3DCodecError::TRAILING_BYTES);

    malformed = *encoded;
    writeU32(malformed, 0u, 0u);
    expectError(malformed, EStaticColliderBatch3DCodecError::BAD_MAGIC);

    malformed = *encoded;
    writeU32(malformed, 4u, kStaticColliderBatch3DSchemaVersion + 1u);
    expectError(
        malformed,
        EStaticColliderBatch3DCodecError::UNSUPPORTED_VERSION);

    malformed = *encoded;
    writeU32(
        malformed,
        8u,
        kStaticColliderBatch3DMaximumHeightfields + 1u);
    expectError(
        malformed,
        EStaticColliderBatch3DCodecError::LIMIT_EXCEEDED);

    malformed = *encoded;
    writeU32(malformed, 12u, 1u);
    expectError(malformed, EStaticColliderBatch3DCodecError::INVALID_LAYOUT);

    malformed = *encoded;
    writeU32(malformed, 40u, 2u);
    expectError(malformed, EStaticColliderBatch3DCodecError::INVALID_LAYOUT);

    auto invalid = source;
    invalid.heightfields.front().sample_spacing =
        std::numeric_limits<float>::quiet_NaN();
    const auto invalid_value = encodeStaticColliderBatch3DBlob(invalid);
    assert(!invalid_value);
    assert(invalid_value.error().error ==
        EStaticColliderBatch3DCodecError::INVALID_VALUE);
    return 0;
}

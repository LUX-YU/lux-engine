#include <lux/engine/simulation/SimulationAssetCodec.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <limits>
#include <memory>
#include <utility>

int main()
{
    using namespace lux::asset;
    using namespace lux::simulation;

    auto code_lifetime = std::make_shared<int>(7);
    auto descriptor = simulationAssetCodecDescriptor(code_lifetime);
    assert(
        descriptor.type == AssetTypeId::fromName(SimulationAssetCanonicalName)
    );
    assert(descriptor.canonical_name == SimulationAssetCanonicalName);
    assert(descriptor.primary_magic == SimulationAssetPrimaryMagic);
    assert(descriptor.legacy_magic == 0U);
    assert(
        descriptor.cpp_payload_type ==
        lux::cxx::typeToken<SimulationDescription>()
    );
    assert(code_lifetime.use_count() == 2U);

    SimulationDescriptionBuilder builder;
    const auto schema_a = simulationDataSchemaId("test.aa");
    const auto schema_b = simulationDataSchemaId("test.bb");
    const std::array payload_a{std::byte{1U}, std::byte{2U}};
    const std::array payload_b{std::byte{3U}};
    assert(builder.addData(schema_b, 2U, payload_b));
    assert(builder.addData(schema_a, 1U, payload_a));
    auto description = std::move(builder).build();
    assert(description);

    auto encoded = descriptor.encode(
        std::addressof(*description),
        AssetEncodeContext{AssetCodecLimits{
            0U,
            0U,
            std::numeric_limits<std::size_t>::max()}}
    );
    assert(encoded);
    assert(!descriptor.encode(
        std::addressof(*description),
        AssetEncodeContext{AssetCodecLimits{0U, 0U, encoded->size() - 1U}}
    ));

    const AssetDecodeContext generous_decode{AssetCodecLimits{
        encoded->size(),
        std::numeric_limits<std::size_t>::max(),
        0U}};
    auto decoded = descriptor.decode(*encoded, generous_decode);
    assert(decoded);
    auto decoded_description =
        std::static_pointer_cast<const SimulationDescription>(decoded->payload);
    assert(decoded_description);
    assert(decoded_description->dataCount() == 2U);
    assert(decoded_description->findData(schema_a));
    assert(decoded_description->findData(schema_b));
    assert(
        decoded->decoded_byte_count == decoded_description->retainedBytes()
    );
    assert(!descriptor.decode(
        *encoded,
        AssetDecodeContext{AssetCodecLimits{
            encoded->size() - 1U,
            std::numeric_limits<std::size_t>::max(),
            0U}}
    ));
    assert(!descriptor.decode(
        *encoded,
        AssetDecodeContext{AssetCodecLimits{encoded->size(), 1U, 0U}}
    ));

    auto trailing = *encoded;
    trailing.push_back(std::byte{});
    assert(!descriptor.decode(
        trailing,
        AssetDecodeContext{AssetCodecLimits{
            trailing.size(),
            std::numeric_limits<std::size_t>::max(),
            0U}}
    ));
    auto corrupt_magic = *encoded;
    corrupt_magic[0] ^= std::byte{0x01U};
    assert(!descriptor.decode(corrupt_magic, generous_decode));
    auto truncated = *encoded;
    truncated.pop_back();
    assert(!descriptor.decode(truncated, generous_decode));

    auto noncanonical = *encoded;
    constexpr std::size_t header_bytes = 40U;
    constexpr std::size_t schema_record_bytes = 16U + 7U;
    for (std::size_t index{}; index < schema_record_bytes; ++index)
    {
        std::swap(
            noncanonical[header_bytes + index],
            noncanonical[header_bytes + schema_record_bytes + index]
        );
    }
    assert(!descriptor.decode(noncanonical, generous_decode));

    return 0;
}

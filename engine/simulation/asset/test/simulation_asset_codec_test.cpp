#include <lux/engine/simulation/SimulationAssetCodec.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/asset/SystemDescriptionCompatibility.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <limits>
#include <memory>
#include <utility>

namespace
{
    struct CollisionEvent final
    {
        std::uint32_t body{};
    };

    inline constexpr std::array kPhysicsCapabilities{
        std::string_view{"physics.simulate"}};
    inline constexpr std::array kPhysicsPoints{
        lux::simulation::SystemExecutionPoint{"before"},
        lux::simulation::SystemExecutionPoint{"after"}};
    inline constexpr std::array kPhysicsEvents{
        lux::simulation::makeSystemEvent<CollisionEvent>(
            "collision",
            kPhysicsPoints[1],
            "lux.event.Collision",
            1U
        )};
    struct PhysicsSystem final
    {
        inline static constexpr lux::simulation::SystemDescription Description{
            .canonical_name = "lux.physics",
            .version = 3U,
            .configuration_schema_name = "lux.physics.Config",
            .configuration_schema_version = 2U,
            .capabilities = kPhysicsCapabilities,
            .execution_points = kPhysicsPoints,
            .events = kPhysicsEvents};
    };
}

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

    constexpr std::array physics_capabilities{
        std::string_view{"physics.simulate"}};
    constexpr std::array physics_points{
        SystemExecutionPoint{"before"},
        SystemExecutionPoint{"after"}};
    constexpr std::array physics_events{
        makeSystemEvent<CollisionEvent>(
            "collision",
            physics_points[1],
            "lux.event.Collision",
            1U
        )};
    const SystemDescription physics{
        .canonical_name = "lux.physics",
        .version = 3U,
        .configuration_schema_name = "lux.physics.Config",
        .configuration_schema_version = 2U,
        .capabilities = physics_capabilities,
        .execution_points = physics_points,
        .events = physics_events};

    constexpr std::array animation_capabilities{
        std::string_view{"animation.evaluate"}};
    constexpr std::array animation_points{
        SystemExecutionPoint{"before"},
        SystemExecutionPoint{"after"}};
    constexpr std::array animation_events{
        makeSystemEvent<void>(
            "finished",
            animation_points[1],
            "ignored.for.void",
            99U
        )};
    const SystemDescription animation{
        .canonical_name = "lux.animation",
        .version = 5U,
        .capabilities = animation_capabilities,
        .execution_points = animation_points,
        .events = animation_events};
    constexpr std::array physics_configuration{
        std::byte{0x11U}, std::byte{0x22U}};
    assert(builder.addSystem("physics", physics, physics_configuration));
    assert(builder.addSystem("animation", animation));
    assert(builder.addDependency(
        "physics",
        physics_points[1],
        "animation",
        animation_points[0]
    ));
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
    assert((*encoded)[4] == std::byte{2U});
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
    assert(decoded_description->systemCount() == 2U);
    assert(decoded_description->dependencyCount() == 1U);
    assert(decoded_description->findData(schema_a));
    assert(decoded_description->findData(schema_b));
    const auto decoded_physics = decoded_description->findSystem("physics");
    const auto decoded_animation = decoded_description->findSystem("animation");
    assert(decoded_physics && decoded_animation);
    assert(
        lux::simulation::asset::matchesCurrentSystemDescription<PhysicsSystem>(
            decoded_physics
        )
    );
    assert(decoded_physics.type() == systemTypeId("lux.physics"));
    assert(decoded_physics.configurationPayload().size() == 2U);
    assert(decoded_physics.hasCapability("physics.simulate"));
    assert(decoded_physics.findEvent("collision"));
    assert(
        decoded_physics.findEvent("collision").payloadSchemaName() ==
        "lux.event.Collision"
    );
    assert(decoded_animation.findEvent("finished"));
    assert(decoded_animation.findEvent("finished").payloadSchemaName().empty());
    assert(
        decoded_description->dependencyAt(0U).before().system().instanceName() ==
        "physics"
    );
    assert(decoded_description->dependencyAt(0U).before().name() == "after");
    assert(
        decoded_description->dependencyAt(0U).after().system().instanceName() ==
        "animation"
    );
    assert(decoded_description->dependencyAt(0U).after().name() == "before");
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

    auto wire_v1 = *encoded;
    wire_v1[4] = std::byte{1U};
    assert(!descriptor.decode(wire_v1, generous_decode));

    auto overlapping_section = *encoded;
    overlapping_section[40] ^= std::byte{0x01U};
    assert(!descriptor.decode(overlapping_section, generous_decode));

    auto reencoded = descriptor.encode(
        decoded_description.get(),
        AssetEncodeContext{AssetCodecLimits{
            0U,
            0U,
            std::numeric_limits<std::size_t>::max()}}
    );
    assert(reencoded);
    assert(*reencoded == *encoded);

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

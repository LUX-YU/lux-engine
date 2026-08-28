#include <lux/engine/simulation/SimulationAssetCodec.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationStepInfo.hpp>
#include <lux/engine/simulation/asset/SystemDescriptionCompatibility.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

struct CollisionEvent final
{
    std::uint32_t body{};
};

namespace lux::semantic
{
    template <>
    struct TypeTraits<CollisionEvent> final
    {
        inline static constexpr std::string_view CanonicalName{
            "lux.event.Collision"};
        inline static constexpr std::uint8_t AbiKind{
            static_cast<std::uint8_t>(EAbiKind::STRUCT_REF)};
        inline static constexpr std::uint32_t Size{sizeof(CollisionEvent)};
        inline static constexpr std::uint32_t Alignment{alignof(CollisionEvent)};
    };
}

namespace
{
    using namespace lux::simulation;

    inline constexpr SystemInstanceId kPhysicsInstance{31U};
    inline constexpr SystemInstanceId kAnimationInstance{32U};
    inline constexpr HookPointId kPhysicsBefore{301U};
    inline constexpr HookPointId kPhysicsAfter{302U};
    inline constexpr EventPointId kCollision{401U};

    inline constexpr std::array kPhysicsCapabilities{
        std::string_view{"physics.simulate"}};
    inline constexpr std::array kPhysicsHooks{
        makeHookPointSpec<void(const SimulationStepInfo&)>(
            kPhysicsBefore,
            "before"
        ),
        makeHookPointSpec<void(const SimulationStepInfo&)>(
            kPhysicsAfter,
            "after"
        )};
    inline constexpr std::array kPhysicsEvents{
        makeEventPointSpec<CollisionEvent>(
            kCollision,
            "collision",
            kPhysicsAfter,
            EEventRoute::ENTITY_TARGETED,
            "lux.event.Collision",
            1U
        )};

    struct PhysicsSystem final
    {
        inline static constexpr SystemDescription Description{
            .canonical_name = "lux.physics",
            .version = 3U,
            .configuration_schema_name = "lux.physics.Config",
            .configuration_schema_version = 2U,
            .capabilities = kPhysicsCapabilities,
            .hooks = kPhysicsHooks,
            .events = kPhysicsEvents};
    };
}

int main()
{
    using namespace lux::asset;
    using namespace lux::simulation;

    auto code_lifetime = std::make_shared<int>(7);
    auto descriptor = simulationAssetCodecDescriptor(code_lifetime);
    assert(descriptor.type ==
        AssetTypeId::fromName(SimulationAssetCanonicalName));
    assert(descriptor.primary_magic == SimulationAssetPrimaryMagic);
    assert(descriptor.legacy_magic == 0U);
    assert(descriptor.cpp_payload_type ==
        lux::cxx::typeToken<SimulationDescription>());

    SimulationDescriptionBuilder builder;
    const auto schema_a = simulationDataSchemaId("test.aa");
    const auto schema_b = simulationDataSchemaId("test.bb");
    const std::array payload_a{std::byte{1U}, std::byte{2U}};
    const std::array payload_b{std::byte{3U}};
    assert(builder.addData(schema_b, 2U, payload_b));
    assert(builder.addData(schema_a, 1U, payload_a));

    constexpr std::array physics_configuration{
        std::byte{0x11U}, std::byte{0x22U}};
    assert(builder.addSystem(
        kPhysicsInstance,
        "physics",
        PhysicsSystem::Description,
        physics_configuration
    ));
    constexpr std::array<HookPointSpec, 0U> no_hooks{};
    constexpr std::array<EventPointSpec, 0U> no_events{};
    const SystemDescription animation{
        .canonical_name = "lux.animation",
        .version = 5U,
        .hooks = no_hooks,
        .events = no_events};
    assert(builder.addSystem(kAnimationInstance, "animation", animation));
    assert(builder.addDependency(kPhysicsInstance, kAnimationInstance));
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
    assert((*encoded)[4] == std::byte{5U});
    assert(!descriptor.encode(
        std::addressof(*description),
        AssetEncodeContext{AssetCodecLimits{0U, 0U, encoded->size() - 1U}}
    ));

    const AssetDecodeContext generous{AssetCodecLimits{
        encoded->size(),
        std::numeric_limits<std::size_t>::max(),
        0U}};
    auto decoded = descriptor.decode(*encoded, generous);
    assert(decoded);
    auto value = std::static_pointer_cast<const SimulationDescription>(
        decoded->payload);
    assert(value && value->dataCount() == 2U && value->systemCount() == 2U);
    assert(value->dependencyCount() == 1U);
    const auto physics = value->findSystem(kPhysicsInstance);
    assert(physics && physics.instanceName() == "physics");
    assert(lux::simulation::asset::matchesCurrentSystemDescription<
        PhysicsSystem>(physics));
    assert(physics.findHookPoint(kPhysicsAfter));
    assert(physics.findEvent(kCollision).route() ==
        EEventRoute::ENTITY_TARGETED);
    assert(value->dependencyAt(0U).after().instanceId() ==
        kAnimationInstance);
    assert(decoded->decoded_byte_count == value->retainedBytes());

    auto reencoded = descriptor.encode(
        value.get(),
        AssetEncodeContext{AssetCodecLimits{
            0U,
            0U,
            std::numeric_limits<std::size_t>::max()}}
    );
    assert(reencoded && *reencoded == *encoded);

    for (const auto old_version : {1U, 2U, 3U, 4U})
    {
        auto old_wire = *encoded;
        old_wire[4] = static_cast<std::byte>(old_version);
        assert(!descriptor.decode(old_wire, generous));
    }
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
    corrupt_magic[0] ^= std::byte{1U};
    assert(!descriptor.decode(corrupt_magic, generous));
    auto corrupt_directory = *encoded;
    corrupt_directory[40] ^= std::byte{1U};
    assert(!descriptor.decode(corrupt_directory, generous));
    assert(!descriptor.decode(
        *encoded,
        AssetDecodeContext{AssetCodecLimits{
            encoded->size() - 1U,
            std::numeric_limits<std::size_t>::max(),
            0U}}
    ));
    return 0;
}

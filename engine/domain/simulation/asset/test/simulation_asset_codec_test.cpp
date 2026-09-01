#include <lux/engine/simulation/SimulationAssetCodec.hpp>
#include <lux/engine/resource/asset/CookedAssetImage.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationStepInfo.hpp>
#include <lux/engine/simulation/asset/SimulationSystemDescriptionValidation.hpp>
#include <lux/cxx/algorithm/sha256.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <iostream>

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

    inline constexpr lux::system::SystemInstanceId kPhysicsInstance{31U};
    inline constexpr lux::system::SystemInstanceId kAnimationInstance{32U};
    inline constexpr HookPointId kPhysicsBefore{301U};
    inline constexpr HookPointId kPhysicsAfter{302U};
    inline constexpr EventPointId kCollision{401U};

    [[nodiscard]] lux::asset::AssetId assetId(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes.back() = tail;
        return lux::asset::AssetId{bytes};
    }

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
        inline static constexpr SimulationSystemDescription Description{
            .type = {
                .canonical_name = "lux.physics",
                .version = 3U,
                .configuration_schema_name = "lux.physics.Config",
                .configuration_schema_version = 2U,
                .capabilities = kPhysicsCapabilities
            },
            .hooks = kPhysicsHooks,
            .events = kPhysicsEvents};
    };
}

int main()
{
    using namespace lux::asset;
    using namespace lux::simulation;

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
    const SimulationSystemDescription animation{
        .type = {
            .canonical_name = "lux.animation",
            .version = 5U
        },
        .hooks = no_hooks,
        .events = no_events};
    assert(builder.addSystem(kAnimationInstance, "animation", animation));
    assert(builder.addDependency(kPhysicsInstance, kAnimationInstance));
    auto description = std::move(builder).build();
    assert(description);
    auto description_owner = std::make_shared<const SimulationDescription>(std::move(*description));
    auto asset = SimulationAsset::create(
        AssetInfo{assetId(7U), SimulationAsset::asset_type, 23U},
        description_owner
    );
    assert(asset);
    constexpr AssetEncodeLimits encode_limits{1024U * 1024U};
    constexpr AssetDecodeLimits decode_limits{1024U * 1024U, 1024U * 1024U, 4U};
    auto encoded = TAssetSerDeser<SimulationAsset>::encode(**asset, encode_limits);
    assert(encoded);
    const auto outer = inspectCookedAssetImage(
        (*asset)->id(),
        lux::cxx::SharedBytes<>::copyOf(*encoded),
        decode_limits
    );
    assert(outer && outer->information().empty() && outer->data().size() == 873U);
    const auto digest = lux::cxx::algorithm::Sha256::hash(outer->data().view());
    const auto expected_digest = lux::cxx::algorithm::Sha256Digest::fromHex(
        "480ed360a99aac9ac125915739e38469896c53bd9d14f76efaac8b6b8f4ac8f3"
    );
    assert(expected_digest && digest == *expected_digest);
    assert(outer->data().view()[4] == std::byte{6U});
    assert(!TAssetSerDeser<SimulationAsset>::encode(
        **asset,
        AssetEncodeLimits{encoded->size() - 1U}
    ));

    auto decoded = TAssetSerDeser<SimulationAsset>::decode(
        (*asset)->id(),
        lux::cxx::SharedBytes<>::copyOf(*encoded),
        decode_limits
    );
    assert(decoded);
    const auto& value = (*decoded)->data();
    assert(value.dataCount() == 2U && value.systemCount() == 2U);
    assert(value.dependencyCount() == 1U);
    const auto physics = value.findSystem(kPhysicsInstance);
    assert(physics && physics.instanceName() == "physics");
    assert(lux::simulation::asset::matchesCurrentSystemDescription<
        PhysicsSystem>(physics));
    assert(physics.findHookPoint(kPhysicsAfter));
    assert(physics.findEvent(kCollision).route() ==
        EEventRoute::ENTITY_TARGETED);
    assert(value.dependencyAt(0U).after().instanceId() ==
        kAnimationInstance);

    auto reencoded = TAssetSerDeser<SimulationAsset>::encode(**decoded, encode_limits);
    assert(reencoded && *reencoded == *encoded);

    for (const auto old_version : {1U, 2U, 3U, 4U, 5U})
    {
        auto old_wire = *encoded;
        old_wire[404U] = static_cast<std::byte>(old_version);
        assert(!TAssetSerDeser<SimulationAsset>::decode(
            (*asset)->id(),
            lux::cxx::SharedBytes<>::copyOf(old_wire),
            decode_limits
        ));
    }
    auto trailing = *encoded;
    trailing.push_back(std::byte{});
    assert(!TAssetSerDeser<SimulationAsset>::decode(
        (*asset)->id(),
        lux::cxx::SharedBytes<>::copyOf(trailing),
        AssetDecodeLimits{trailing.size(), trailing.size(), 4U}
    ));
    auto corrupt_magic = *encoded;
    corrupt_magic[400U] ^= std::byte{1U};
    assert(!TAssetSerDeser<SimulationAsset>::decode(
        (*asset)->id(),
        lux::cxx::SharedBytes<>::copyOf(corrupt_magic),
        decode_limits
    ));
    auto corrupt_directory = *encoded;
    corrupt_directory[440U] ^= std::byte{1U};
    assert(!TAssetSerDeser<SimulationAsset>::decode(
        (*asset)->id(),
        lux::cxx::SharedBytes<>::copyOf(corrupt_directory),
        decode_limits
    ));
    assert(!TAssetSerDeser<SimulationAsset>::decode(
        assetId(8U),
        lux::cxx::SharedBytes<>::copyOf(*encoded),
        decode_limits
    ));
    return 0;
}

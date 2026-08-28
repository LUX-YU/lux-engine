#include <lux/engine/simulation/SimulationStepInfo.hpp>
#include <lux/engine/simulation/systems/ScriptSystemDescriptionCodec.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    inline constexpr SystemInstanceId kSystem{81U};
    inline constexpr HookPointId kHook{82U};
    inline constexpr EventPointId kBroadcastEvent{83U};
    inline constexpr EventPointId kTargetedEvent{84U};

    [[nodiscard]] SimulationDescription makeSimulation()
    {
        constexpr std::array hooks{
            makeHookPointSpec<void(const SimulationStepInfo&)>(
                kHook,
                "tick"
            )};
        constexpr std::array events{
            makeEventPointSpec<SimulationStepInfo>(
                kBroadcastEvent,
                "broadcast",
                kHook,
                EEventRoute::SIMULATION_BROADCAST,
                "lux.simulation.SimulationStepInfo",
                1U
            ),
            makeEventPointSpec<SimulationStepInfo>(
                kTargetedEvent,
                "targeted",
                kHook,
                EEventRoute::ENTITY_TARGETED,
                "lux.simulation.SimulationStepInfo",
                1U
            )};
        const SystemDescription description{
            .canonical_name = "lux.test.system",
            .version = 1U,
            .hooks = hooks,
            .events = events};
        SimulationDescriptionBuilder builder;
        assert(builder.addSystem(kSystem, "test", description));
        auto built = std::move(builder).build();
        assert(built);
        return std::move(*built);
    }

    [[nodiscard]] lux::asset::AssetId makeAsset(std::uint8_t seed)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[0] = seed;
        return lux::asset::AssetId{bytes};
    }

    [[nodiscard]] lux::world::WorldObjectId makeObject(std::uint8_t seed)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[0] = seed;
        return lux::world::WorldObjectId{uuids::uuid{bytes}};
    }
}

int main()
{
    using namespace lux::simulation::script;
    const auto simulation = makeSimulation();

    ScriptSystemDescriptionBuilder builder;
    assert(builder.addMount(ScriptMountDescription{
        ScriptMountId{1U},
        makeAsset(1U),
        SimulationScriptMount{},
        true,
        {{7U, HookScriptTarget{kSystem, kHook}},
         {8U, EventScriptTarget{kSystem, kBroadcastEvent}}}}
    ));
    assert(builder.addMount(ScriptMountDescription{
        ScriptMountId{2U},
        makeAsset(2U),
        EntityScriptMount{makeObject(2U)},
        false,
        {{9U, EventScriptTarget{kSystem, kTargetedEvent}}}}
    ));
    auto description = std::move(builder).build(simulation);
    assert(description && description->mounts().size() == 2U);

    const ScriptSystemCodecLimits generous{
        std::numeric_limits<std::size_t>::max(),
        std::numeric_limits<std::size_t>::max(),
        std::numeric_limits<std::size_t>::max()};
    auto encoded = encodeScriptSystemDescription(*description, generous);
    assert(encoded);
    assert((*encoded)[4] == std::byte{1U});
    auto decoded = decodeScriptSystemDescription(
        *encoded,
        simulation,
        generous
    );
    assert(decoded &&
        decoded->mounts().size() == description->mounts().size() &&
        std::equal(
            decoded->mounts().begin(),
            decoded->mounts().end(),
            description->mounts().begin()
        ));
    auto reencoded = encodeScriptSystemDescription(*decoded, generous);
    assert(reencoded && *reencoded == *encoded);

    auto old_version = *encoded;
    old_version[4] = std::byte{0U};
    assert(!decodeScriptSystemDescription(old_version, simulation, generous));
    auto corrupt_scope = *encoded;
    corrupt_scope[80U + 24U] = std::byte{2U};
    assert(!decodeScriptSystemDescription(corrupt_scope, simulation, generous));

    ScriptSystemDescriptionBuilder invalid_scope;
    assert(invalid_scope.addMount(ScriptMountDescription{
        ScriptMountId{3U},
        makeAsset(3U),
        SimulationScriptMount{},
        true,
        {{10U, EventScriptTarget{kSystem, kTargetedEvent}}}}
    ));
    const auto rejected = std::move(invalid_scope).build(simulation);
    assert(!rejected);
    assert(rejected.error() == EScriptSystemDescriptionError::SCOPE_MISMATCH);

    SimulationDescriptionBuilder simulation_builder;
    assert(addScriptSystemData(
        simulation_builder,
        *description,
        generous
    ));
    auto with_data = std::move(simulation_builder).build();
    assert(with_data);
    const auto data = with_data->findData(scriptSystemDataSchemaId());
    assert(data && data.version() == ScriptSystemDescription::kSchemaVersion);
    return 0;
}

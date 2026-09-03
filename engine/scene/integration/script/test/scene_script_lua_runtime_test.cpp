#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/scene/Scene.hpp>
#include <lux/engine/scene/SceneBuilder.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/ScriptRuntimeSystem.hpp>
#include <lux/engine/simulation/SimulationBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ScriptSystemDescriptionCodec.hpp>
#include <lux/engine/simulation/abilities/DelayAbility.hpp>
#include "DelayAbility.ability.generated.hpp"
#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace
{
    using namespace lux;
    using namespace lux::scene;
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    inline constexpr system::SystemInstanceId kProbeSystem{0x4C5301U};
    inline constexpr system::SystemInstanceId kScriptRuntime{0x4C5302U};
    inline constexpr HookPointId kTickHook{0x4C5303U};
    inline constexpr lux::script::ScriptSymbolId kTickSymbol{0x4C5304U};
    using DelayAbility = lux::simulation::script::DelayAbility;
    using DelayTraits = lux::script::ScriptAbilityTraits<DelayAbility>;

    [[nodiscard]] asset::AssetId assetId(std::uint8_t value)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[0] = value;
        return asset::AssetId{bytes};
    }

    template <class Type>
    [[nodiscard]] Type worldId(std::uint8_t value)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[15] = value;
        return Type{uuids::uuid{bytes}};
    }

    struct ProbeSystem final
    {
        inline static constexpr auto Access = makeSystemAccessSpec<>();
        inline static constexpr std::array Hooks{makeHookPointSpec<void()>(kTickHook, "lua-tick")};
        inline static constexpr SimulationSystemDescription Description{
            .type = {.canonical_name = "lux.test.scene-lua.probe", .version = 1U},
            .hooks = Hooks
        };

        ProbeSystem() noexcept : endpoint(kProbeSystem, kTickHook, hook)
        {
            ready = hook.prepare(1U) == EEndpointMutationError::NONE;
        }

        void execute() noexcept
        {
            static_cast<void>(hook.dispatch());
        }

        HookPoint<void()> hook;
        ScriptHookEndpoint<void()> endpoint;
        bool ready{};
    };

    [[nodiscard]] lux::cxx::expected<void, SimulationSystemBuildFailure> installProbe(
        SimulationBuilder& builder,
        SimulationSystemView description
    ) noexcept
    {
        auto probe = builder.emplaceSystem<ProbeSystem>(description.instanceId());
        if (!probe)
            return lux::cxx::unexpected(probe.error());
        if (!(*probe)->ready)
        {
            return lux::cxx::unexpected(SimulationSystemBuildFailure{
                ESimulationSystemBuildError::CONSTRUCTION_FAILURE,
                description.instanceId()
            });
        }
        auto published = builder.publishScriptHook(description.instanceId(), (*probe)->endpoint.descriptor());
        if (!published)
            return published;
        return builder.addSystemTask<ProbeSystem>(description.instanceId(), [](ProbeSystem& value) noexcept {
            value.execute();
        });
    }

    [[nodiscard]] SimulationSystemRegistration probeRegistration() noexcept
    {
        return {
            .type = system::systemTypeId(ProbeSystem::Description.type.canonical_name),
            .cpp_type = cxx::typeToken<ProbeSystem>(),
            .description = &ProbeSystem::Description,
            .access = ProbeSystem::Access.spec(),
            .configuration = {},
            .install = &installProbe
        };
    }

    [[nodiscard]] ScriptSystemDescription makeScriptDescription(const SimulationDescription& simulation)
    {
        ScriptSystemDescriptionBuilder builder;
        assert(builder.addMount({
            ScriptMountId{1U},
            assetId(0x4CU),
            SimulationScriptMount{},
            true,
            {{kTickSymbol, HookScriptTarget{kProbeSystem, kTickHook}}}
        }));
        auto description = std::move(builder).build(simulation);
        assert(description);
        return std::move(*description);
    }

    [[nodiscard]] std::shared_ptr<const SimulationDescription> makeSimulationDescription(
        const ScriptSystemCodecLimits& limits
    )
    {
        SimulationDescriptionBuilder base_builder;
        assert(base_builder.addSystem(kProbeSystem, "probe", ProbeSystem::Description));
        auto base = std::move(base_builder).build();
        assert(base);
        auto script = makeScriptDescription(*base);

        SimulationDescriptionBuilder builder;
        assert(builder.addSystem(kProbeSystem, "probe", ProbeSystem::Description));
        assert(addScriptSystemData(builder, script, limits));
        auto result = std::move(builder).build();
        assert(result);
        return std::make_shared<SimulationDescription>(std::move(*result));
    }

    [[nodiscard]] lux::script::ScriptArtifact makeArtifact()
    {
        constexpr std::string_view source = R"lua(
            return {
                tick = function()
                    lux.Delay.nextStep()
                    error("lua nextStep resumed at the production stable point")
                end
            }
        )lua";
        rdesc::Script description;
        description.module_name = "lux.test.scene-lua.fixture";
        description.exports.push_back({"tick", kTickSymbol, {}, {}});
        description.api_requirements.push_back({
            lux::script::ScriptApiContractId{DelayTraits::Description.id.name()},
            DelayTraits::Description.schema_hash
        });
        description.body = rdesc::LuaSourceScript{"SceneLuaFixture", {kTickSymbol}};
        std::vector<std::byte> payload;
        payload.reserve(source.size());
        for (const auto value : source)
            payload.push_back(static_cast<std::byte>(value));
        auto artifact = lux::script::ScriptArtifact::create(std::move(description), std::move(payload));
        assert(artifact);
        return std::move(*artifact);
    }

    [[nodiscard]] std::shared_ptr<const world::WorldDescription> makeWorld()
    {
        world::WorldDescriptionBuilder builder;
        assert(builder.setIdentity(
            worldId<world::WorldBundleId>(1U),
            worldId<world::WorldBundleGeneration>(2U),
            "scene-lua-runtime-test"
        ));
        assert(builder.setPartitioner({world::worldPartitionerId("test.none"), 1U}, 0U));
        auto world = std::move(builder).build();
        assert(world);
        return std::make_shared<world::WorldDescription>(std::move(*world));
    }

    struct Fixture final
    {
        Fixture() : artifact(makeArtifact())
        {
            contribution = lux::script::lua::makeScriptAbilityLuaContribution<DelayAbility>();
            auto created = LuaScriptBackend::create({1U, 2U, 1U, 4U, 4U, {}, {}, {&contribution, 1U}});
            assert(created);
            backend.emplace(std::move(*created));
            descriptor = backend->descriptor();
        }

        static bool resolve(
            void* context,
            const asset::AssetId& requested,
            ResolvedScriptArtifact& result
        ) noexcept
        {
            auto& self = *static_cast<Fixture*>(context);
            if (requested != assetId(0x4CU))
                return false;
            result.artifact = std::addressof(self.artifact);
            return true;
        }

        lux::script::ScriptArtifact artifact;
        lux::script::lua::ScriptAbilityLuaContribution contribution;
        std::optional<LuaScriptBackend> backend;
        ScriptBackendDescriptor descriptor;
    };
} // namespace

int main()
{
    const ScriptSystemCodecLimits codec_limits{4096U, 4096U, 4096U};
    auto simulation = makeSimulationDescription(codec_limits);
    SceneDescriptionBuilder description_builder;
    description_builder.setWorld(assetId(1U));
    description_builder.setSimulation(assetId(2U));
    assert(description_builder.addSystem(
        kScriptRuntime,
        "script-runtime",
        system::systemTypeId(ScriptRuntimeSystem::Description.canonical_name),
        ScriptRuntimeSystem::Description.version,
        {},
        0U
    ));
    assert(description_builder.bindRequirement(kScriptRuntime, "script_runtime_host", "host.script"));
    assert(description_builder.bindRequirement(kScriptRuntime, "timer", "host.timer"));
    auto scene_description = std::move(description_builder).build();
    assert(scene_description);

    meta::ReflectionRegistry::initRegistry();
    SimulationSystemRegistry simulation_systems;
    assert(simulation_systems.add(probeRegistration()));
    auto components = ecs::ComponentSchemaSet::build({});
    assert(components);
    auto meta = SceneMetaManager::build({
        std::move(*components),
        std::move(simulation_systems),
        {builtinScriptRuntimeSystemRegistration()}
    });
    assert(meta);

    Fixture fixture;
    const std::array backends{fixture.descriptor};
    ScriptRuntimeHost host{
        {8U, 1U, 2U, 2U, 2U, 2U, 64U, 2U, 2U, 2U},
        codec_limits,
        2U,
        {&fixture, &Fixture::resolve},
        {},
        backends,
        {}
    };
    auto execution = process::ExecutionRuntime::create({1U, 8U, 8U, {8U}, std::nullopt});
    assert(execution);
    auto timer = execution->timer();
    const std::array providers{
        makeSceneCapabilityProvider<ScriptRuntimeHost>("host.script", "lux.script.runtime.host", host),
        makeSceneCapabilityProvider<process::TimerClient>("host.timer", "lux.process.timer", timer)
    };
    auto scene = Scene::create({
        std::make_shared<SceneDescription>(std::move(*scene_description)),
        makeWorld(),
        simulation,
        *meta,
        providers
    });
    assert(scene);
    auto* runtime = (*scene)->findSceneSystem<ScriptRuntimeSystem>();
    assert(runtime != nullptr);
    auto executor = task::TaskExecutor::create({1U, 8U});
    assert(executor);

    assert((*scene)->simulation().execute(*executor, SimulationDuration{1}));
    assert(runtime->scriptSystem().activeContinuationCount() == 1U);
    assert(runtime->scriptSystem().failures().empty());
    assert((*scene)->executeStablePoint());
    assert(runtime->scriptSystem().activeContinuationCount() == 1U);
    assert(runtime->scriptSystem().failures().empty());
    assert((*scene)->simulation().execute(*executor, SimulationDuration{1}));
    const auto stable = (*scene)->executeStablePoint();
    assert(!stable);
    assert(runtime->scriptSystem().activeContinuationCount() == 0U);
    assert(!runtime->scriptSystem().failures().empty());
    assert(runtime->scriptSystem().failures().back().error == EScriptSystemError::INVOCATION_FAILURE);

    scene->reset();
    execution->requestStop();
    assert(execution->join());
    meta::ReflectionRegistry::destroyRegistry();
    return 0;
}

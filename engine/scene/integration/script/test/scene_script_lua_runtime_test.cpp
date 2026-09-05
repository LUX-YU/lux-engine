#include "LuaRuntimeTestAbility.hpp"
#include "LuaRuntimeTestAbility.ability.generated.hpp"
#include "LuaRuntimeTestAbility.ability.lua.generated.hpp"

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
#include "DelayAbility.ability.lua.generated.hpp"
#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>

#include <array>
#include <barrier>
#include <thread>
#include <type_traits>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace
{
    lux::script::lua::ELuaExecutionPolicy g_execution_policy{
        lux::script::lua::ELuaExecutionPolicy::DEFAULT
    };
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
    using TestAbility = lux::simulation::script::test::LuaRuntimeTestAbility;
    using TestAbilityTraits = lux::script::ScriptAbilityTraits<TestAbility>;

    struct ProbeSystem;
    ProbeSystem* g_probe_system{};
    std::int32_t g_last_written{};
    std::size_t g_total_writes{};

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
        inline static constexpr std::array Hooks{makeHookPointSpec<void()>(kTickHook, "lua-tick", true, true)};
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
        }

        std::int32_t readValue(std::int32_t input) noexcept
        {
            ++reads;
            return value + input;
        }

        void writeValue(std::int32_t next) noexcept
        {
            ++writes;
            value = next;
            g_last_written = next;
            ++g_total_writes;
        }

        const std::int32_t& borrowValue() noexcept
        {
            return value;
        }

        bool echoBool(bool input) noexcept
        {
            return input;
        }

        std::int32_t echoI32(std::int32_t input) noexcept
        {
            return input;
        }

        std::uint32_t echoU32(std::uint32_t input) noexcept
        {
            return input;
        }

        float echoF32(float input) noexcept
        {
            return input;
        }

        double echoF64(double input) noexcept
        {
            return input;
        }

        lux::script::ScriptAbilityStartResult beginOperation(
            std::int32_t input,
            lux::script::ScriptAbilityCompletion<std::int32_t> completion
        ) noexcept
        {
            ++async_starts;
            const auto completed = completion.success(input + 1);
            return completed
                ? lux::script::ScriptAbilityStartResult{}
                : lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{91});
        }

        HookPoint<void()> hook;
        ScriptHookEndpoint<void()> endpoint;
        std::int32_t value{7};
        std::size_t reads{};
        std::size_t writes{};
        std::size_t async_starts{};
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
        g_probe_system = *probe;
        const auto ability = lux::script::bindScriptAbility<TestAbility>(**probe);
        auto published_ability = builder.publishScriptAbility(description.instanceId(), ability);
        if (!published_ability)
            return published_ability;
        auto published = builder.publishScriptHook(description.instanceId(), (*probe)->endpoint.descriptor());
        if (!published)
            return published;
        auto task = builder.addSystemTask<ProbeSystem>(description.instanceId(), [](ProbeSystem& value) noexcept {
            value.execute();
        });
        if (!task)
            return task;
        return builder.addSystemHookTask<ProbeSystem>(description.instanceId(), kTickHook,
            [](ProbeSystem& value, const HookInvocation& invocation) noexcept {
                static_cast<void>(value.hook.dispatch(invocation));
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

    [[nodiscard]] ScriptSystemDescription makeScriptDescription(
        const SimulationDescription& simulation,
        asset::AssetId script_asset
    )
    {
        ScriptSystemDescriptionBuilder builder;
        assert(builder.addMount({
            ScriptMountId{1U},
            script_asset,
            SimulationScriptMount{},
            true,
            {{kTickSymbol, HookScriptTarget{kProbeSystem, kTickHook}}}
        }));
        auto description = std::move(builder).build(simulation);
        assert(description);
        return std::move(*description);
    }

    [[nodiscard]] std::shared_ptr<const SimulationDescription> makeSimulationDescription(
        const ScriptSystemCodecLimits& limits,
        asset::AssetId script_asset
    )
    {
        SimulationDescriptionBuilder base_builder;
        assert(base_builder.addSystem(kProbeSystem, "probe", ProbeSystem::Description));
        auto base = std::move(base_builder).build();
        assert(base);
        auto script = makeScriptDescription(*base, script_asset);

        SimulationDescriptionBuilder builder;
        assert(builder.addSystem(kProbeSystem, "probe", ProbeSystem::Description));
        assert(builder.addExecutionDependency(SimulationExecutionPoint::task(kProbeSystem),
            SimulationExecutionPoint::hook(kProbeSystem, kTickHook)));
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

    [[nodiscard]] std::shared_ptr<const lux::script::ScriptArtifactAsset> makeArtifactAsset()
    {
#if defined(LUX_LUA_PORTABILITY_ARTIFACT)
        std::ifstream input(LUX_LUA_PORTABILITY_ARTIFACT, std::ios::binary);
        assert(input);
        input.seekg(0, std::ios::end);
        const auto encoded_size = input.tellg();
        assert(encoded_size >= std::streamoff{56});
        input.seekg(0, std::ios::beg);
        std::vector<std::byte> bytes(static_cast<std::size_t>(encoded_size));
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        assert(input);
        std::array<std::uint8_t, 16U> id_bytes{};
        for (std::size_t index{}; index < id_bytes.size(); ++index)
            id_bytes[index] = std::to_integer<std::uint8_t>(bytes[40U + index]);
        const auto decoded = asset::TAssetSerDeser<lux::script::ScriptArtifactAsset>::decode(
            asset::AssetId{id_bytes},
            cxx::SharedBytes<>::copyOf(bytes),
            {bytes.size(), (std::numeric_limits<std::size_t>::max)(), 0U}
        );
        assert(decoded);
        return *decoded;
#else
        auto artifact = std::make_shared<const lux::script::ScriptArtifact>(makeArtifact());
        auto created = lux::script::ScriptArtifactAsset::create(
            asset::AssetInfo{
                assetId(0x4CU),
                lux::script::ScriptArtifactAsset::asset_type,
                0U
            },
            std::move(artifact)
        );
        assert(created);
        return *created;
#endif
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
        Fixture() : artifact(makeArtifactAsset())
        {
            contributions = {
                lux::script::lua::makeScriptAbilityLuaContribution<DelayAbility>(),
                lux::script::lua::makeScriptAbilityLuaContribution<TestAbility>()
            };
            auto created = LuaScriptBackend::create({
                .instance_capacity = 4U,
                .prepared_call_capacity = 8U,
                .continuation_capacity = 8U,
                .execution_depth_capacity = 8U,
                .ability_catalog_method_capacity =
                    DelayTraits::Description.methods.size() + TestAbilityTraits::Description.methods.size(),
                .prepared_ability_capacity =
                    4U * (DelayTraits::Description.methods.size() + TestAbilityTraits::Description.methods.size()),
                .abilities = contributions,
                .execution_policy = g_execution_policy
            });
            assert(created);
            backend.emplace(std::move(*created));
            assert(g_execution_policy != lux::script::lua::ELuaExecutionPolicy::INTERPRETER_ONLY ||
                !backend->runtimeInfo().jit_enabled);
            descriptor = backend->descriptor();
        }

        static bool resolve(
            void* context,
            const asset::AssetId& requested,
            ResolvedScriptArtifact& result
        ) noexcept
        {
            auto& self = *static_cast<Fixture*>(context);
            if (requested != self.artifact->id())
                return false;
            result.artifact = std::addressof(self.artifact->data());
            return true;
        }

        std::shared_ptr<const lux::script::ScriptArtifactAsset> artifact;
        std::array<lux::script::lua::ScriptAbilityLuaContribution, 2U> contributions;
        std::optional<LuaScriptBackend> backend;
        ScriptBackendDescriptor descriptor;
    };
} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view{argv[1]} == "--interpreter-only")
        g_execution_policy = lux::script::lua::ELuaExecutionPolicy::INTERPRETER_ONLY;
    const ScriptSystemCodecLimits codec_limits{4096U, 4096U, 4096U};
    Fixture fixture;
    auto simulation = makeSimulationDescription(codec_limits, fixture.artifact->id());
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

    const std::array backends{fixture.descriptor};
    ScriptRuntimeHost host{
        {16U, 4U, 16U, 8U, 16U, 16U, 64U, 8U, 16U, 16U, 16U, 16U},
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
    static_assert(std::is_const_v<std::remove_reference_t<decltype(runtime->scriptSystem())>>);
    std::barrier stats_ready{2};
    std::jthread observer([&](std::stop_token stop) {
        ScriptRuntimeStats snapshot;
        assert(runtime->acquireStats(snapshot));
        assert(snapshot.active_continuations == 1U);
        stats_ready.arrive_and_wait();
        while (!stop.stop_requested())
        {
            if (runtime->acquireStats(snapshot))
                assert(snapshot.active_continuations <= 1U);
            std::this_thread::yield();
        }
    });
    stats_ready.arrive_and_wait();
    assert((*scene)->executeStablePoint());
    assert(runtime->scriptSystem().activeContinuationCount() == 1U);
    assert(runtime->scriptSystem().failures().empty());
#if defined(LUX_LUA_PORTABILITY_ARTIFACT)
    assert((*scene)->simulation().execute(*executor, SimulationDuration{1}));
    assert((*scene)->executeStablePoint());
    assert(runtime->scriptSystem().activeContinuationCount() == 1U);
    assert((*scene)->simulation().execute(*executor, SimulationDuration{1}));
    assert((*scene)->executeStablePoint());
    assert(runtime->scriptSystem().activeContinuationCount() == 1U);
    assert((*scene)->simulation().execute(*executor, SimulationDuration{1}));
    assert((*scene)->executeStablePoint());
    assert(runtime->scriptSystem().activeContinuationCount() == 0U);
    assert(runtime->scriptSystem().failures().empty());
    assert(g_probe_system != nullptr);
    assert(g_probe_system->reads == 0U);
    assert(g_probe_system->async_starts == 1U);
    assert(g_probe_system->value == 1234);
#else
    assert(!(*scene)->simulation().execute(*executor, SimulationDuration{1}));
    const auto stable = (*scene)->executeStablePoint();
    assert(stable);
    assert(runtime->scriptSystem().activeContinuationCount() == 0U);
    assert(!runtime->scriptSystem().failures().empty());
    assert(runtime->scriptSystem().failures().back().error == EScriptSystemError::INVOCATION_FAILURE);
#endif

    observer.request_stop();
    observer.join();
    scene->reset();
#if defined(LUX_LUA_PORTABILITY_ARTIFACT)
    assert(g_last_written == -1);
    assert(g_total_writes == 3U);
#endif
    execution->requestStop();
    assert(execution->join());
    meta::ReflectionRegistry::destroyRegistry();
    return 0;
}

#include <lux/engine/scene/script/ScriptRuntimeAssembly.hpp>
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
#include <lux/engine/scene/script/ScriptSystemDescriptionCodec.hpp>
#include <lux/engine/simulation/abilities/DelayAbility.hpp>
#include <lux/engine/simulation/scripting/ScriptLifecycle.hpp>
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
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace
{
    using namespace lux;
    using namespace lux::scene;
    using namespace lux::simulation;
    using namespace lux::simulation::script;
    using namespace lux::scene::script;

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
    ecs::Registry* g_mutation_registry{};
    ecs::Entity g_mutation_target{ecs::NullEntity};
    std::size_t g_live_during_provider{};

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
            // Native System code retains direct component access under its declared schedule.
            if (g_mutation_registry && g_mutation_registry->valid(g_mutation_target))
                ++g_mutation_registry->get<std::int32_t>(g_mutation_target);
        }

        std::int32_t readValue(std::int32_t input) noexcept
        {
            if (g_mutation_registry)
            {
                assert(g_mutation_registry->valid(g_mutation_target));
                ++g_live_during_provider;
            }
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
        explicit Fixture(std::span<const LuaComponentBinding> components = {}) : artifact(makeArtifactAsset())
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
                .components = components,
                .abilities = contributions,
                .prepared_ability_blocks = std::array{
                    lux::simulation::script::LuaPreparedBlockClass{
                        DelayTraits::Description.methods.size() + TestAbilityTraits::Description.methods.size(),
                        4U
                    }
                },
                .prepared_ability_storage_bytes =
                    512U * (
                        DelayTraits::Description.methods.size() + TestAbilityTraits::Description.methods.size()
                    ) + 4096U
            });
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

    void testDeferredScene(const SceneMetaManager& meta, process::TimerClient timer, task::TaskExecutor& executor)
    {
        constexpr std::uint64_t score_type{0x4C53F1U};
        const std::array components{LuaComponentBinding{"score", score_type, lux::semantic::typeId("lux.i32"),
            "lux.i32", LUX_SCRIPT_VK_INT32, sizeof(std::int32_t), alignof(std::int32_t)}};
        const std::array host_components{scriptDeferredComponent<std::int32_t>({score_type,
            lux::semantic::typeId("lux.i32"), "lux.i32", LUX_SCRIPT_VK_INT32})};
        for (const bool fault : {false, true})
        {
            Fixture fixture{components};
            rdesc::Script description;
            description.module_name = "lux.test.scene-deferred";
            description.body = rdesc::LuaSourceScript{"Deferred", {kTickSymbol}};
            constexpr lux::script::ScriptSymbolId begin{0x4C53F2U}, end{0x4C53F3U};
            description.exports = {{"tick", kTickSymbol, {}, {}}, {"begin", begin, {}, {}},
                {"finish", end, {rdesc::makeScriptValueType<EScriptEndPlayReason>()}, {}}};
            description.lifecycle = {begin, end};
            description.api_requirements = {
                {lux::script::ScriptApiContractId{DelayTraits::Description.id.name()},
                    DelayTraits::Description.schema_hash},
                {lux::script::ScriptApiContractId{TestAbilityTraits::Description.id.name()},
                    TestAbilityTraits::Description.schema_hash}
            };
            const std::string source = "return {begin=function(self) lux.LuaRuntimeTest.writeValue(100) end,"
                "finish=function(self,reason) lux.LuaRuntimeTest.writeValue(-1) end,tick=function(self) "
                "local score=self:get_component('score'); lux.Delay.nextStep();"
                "if score==11 then assert(self:get_component('score')==12);"
                "assert(self:patch_component('score',25)); assert(self:destroy());"
                "assert(self:get_component('score')==12); assert(lux.LuaRuntimeTest.readValue(1)==101);" +
                std::string{fault ? "error('after accepted mutations');" : ""} +
                "else assert(score==20); assert(lux.LuaRuntimeTest.readValue(2)==102) end end}";
            const auto bytes = std::as_bytes(std::span{source.data(), source.size()});
            auto artifact = lux::script::ScriptArtifact::create(std::move(description), {bytes.begin(), bytes.end()});
            assert(artifact);
            auto asset = lux::script::ScriptArtifactAsset::create(
                asset::AssetInfo{assetId(0xF0U + fault), lux::script::ScriptArtifactAsset::asset_type, 0U},
                std::make_shared<const lux::script::ScriptArtifact>(std::move(*artifact)));
            assert(asset);
            fixture.artifact = *asset;
            SimulationDescriptionBuilder sim_builder;
            assert(sim_builder.addSystem(kProbeSystem, "probe", ProbeSystem::Description));
            auto base = std::move(sim_builder).build();
            assert(base);
            const std::array objects{worldId<world::WorldObjectId>(0xF1U), worldId<world::WorldObjectId>(0xF2U)};
            ScriptSystemDescriptionBuilder mounts;
            for (std::size_t index{}; index < objects.size(); ++index)
                assert(mounts.addMount({ScriptMountId{index + 1U}, fixture.artifact->id(),
                    EntityScriptMount{objects[index]}, true,
                    {{kTickSymbol, HookScriptTarget{kProbeSystem, kTickHook}}}
                }));
            const auto script = std::move(mounts).build(*base);
            assert(script);
            const ScriptSystemCodecLimits codec_limits{4096U, 4096U, 4096U};
            SimulationDescriptionBuilder simulation_builder;
            assert(simulation_builder.addSystem(kProbeSystem, "probe", ProbeSystem::Description));
            assert(simulation_builder.addExecutionDependency(SimulationExecutionPoint::task(kProbeSystem),
                SimulationExecutionPoint::hook(kProbeSystem, kTickHook)));
            assert(addScriptSystemData(simulation_builder, *script, codec_limits));
            auto simulation = std::move(simulation_builder).build();
            assert(simulation);
            SceneDescriptionBuilder scene_builder;
            scene_builder.setWorld(assetId(1U));
            scene_builder.setSimulation(assetId(2U));
            assert(scene_builder.addSystem(kScriptRuntime, "scripts",
                system::systemTypeId(ScriptRuntimeSystem::Description.canonical_name), 1U, {}, 0U));
            assert(scene_builder.bindRequirement(kScriptRuntime, "script_runtime_host", "host.script"));
            assert(scene_builder.bindRequirement(kScriptRuntime, "timer", "host.timer"));
            auto scene_description = std::move(scene_builder).build();
            assert(scene_description);
            struct Resolution final
            {
                const std::array<world::WorldObjectId, 2U>* objects;
                std::array<ecs::Entity, 2U> entities{ecs::NullEntity, ecs::NullEntity};
            } resolution{&objects};
            const std::array backends{fixture.descriptor};
            ScriptRuntimeHost host{
                {8U, 2U, 8U, 4U, 8U, 8U, 64U, 8U, 8U, 8U, 8U, 8U}, codec_limits, 2U,
                {&fixture, &Fixture::resolve},
                {&resolution, [](void* context, const world::WorldObjectId& object, ecs::Entity& output) noexcept {
                    const auto& value = *static_cast<Resolution*>(context);
                    for (std::size_t index{}; index < value.entities.size(); ++index)
                        if ((*value.objects)[index] == object)
                        {
                            output = value.entities[index];
                            return output != ecs::NullEntity;
                        }
                    return false;
                }}, backends, host_components, {2U, 64U}
            };
            const std::array providers{
                makeSceneCapabilityProvider<ScriptRuntimeHost>("host.script", "lux.script.runtime.host", host),
                makeSceneCapabilityProvider<process::TimerClient>("host.timer", "lux.process.timer", timer)
            };
            const auto writes_before = g_total_writes;
            auto scene = Scene::create({std::make_shared<SceneDescription>(std::move(*scene_description)), makeWorld(),
                std::make_shared<SimulationDescription>(std::move(*simulation)), meta, providers});
            assert(scene);
            auto& registry = (*scene)->registry();
            for (std::size_t index{}; index < resolution.entities.size(); ++index)
            {
                resolution.entities[index] = registry.create();
                registry.emplace<std::int32_t>(
                    resolution.entities[index], static_cast<std::int32_t>((index + 1U) * 10U)
                );
            }
            g_mutation_registry = &registry;
            g_mutation_target = resolution.entities.front();
            g_live_during_provider = 0U;
            auto* runtime = (*scene)->findSceneSystem<ScriptRuntimeSystem>();
            assert(runtime);
            assert((*scene)->simulation().execute(executor, SimulationDuration{1}));
            assert(runtime->scriptSystem().stats().next_step_waits == 2U && g_probe_system->reads == 0U);
            assert((*scene)->simulation().execute(executor, SimulationDuration{1}));
            assert(g_probe_system->reads == 2U && g_live_during_provider == 2U);
            assert(!registry.valid(g_mutation_target) && registry.valid(resolution.entities.back()));
            assert(runtime->scriptSystem().activeInstanceCount() == 1U);
            assert(runtime->scriptSystem().stats().backend_resume_calls == 2U);
            assert(runtime->scriptSystem().stats().invocation_failures == static_cast<std::uint64_t>(fault));
            assert(runtime->scriptSystem().stats().resume_queue_depth == 0U);
            const auto commands = runtime->commandStats();
            assert(commands.accepted == 2U && commands.rejected == 0U && commands.rejected_at_commit == 0U);
            assert(g_total_writes - writes_before == 3U); // Two BeginPlay and the retired/faulted instance's EndPlay.
            g_mutation_registry = nullptr;
            scene->reset();
            assert(g_total_writes - writes_before == 4U && fixture.backend->stats().prepared_ability_slots == 0U);
            std::printf("SCENE_DEFERRED fault=%u resumed=2 provider=2 accepted=2 endplay=2 backlog=0 PASS\n", fault);
        }
    }
} // namespace

int main(int argc, char** argv)
{
    std::uint32_t workers{1U};
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument{argv[index]};
        if (argument == "--workers" && index + 1 < argc)
        {
            const std::string_view value{argv[++index]};
            if (value != "0" && value != "1" && value != "2" && value != "4")
                return 2;
            workers = static_cast<std::uint32_t>(value.front() - '0');
        }
        else
            return 2;
    }
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
    auto executor = task::TaskExecutor::create({workers, 8U});
    assert(executor);

#if defined(LUX_LUA_PORTABILITY_ARTIFACT)
    struct ExpectedStep final
    {
        std::uint64_t step;
        std::uint64_t resumes;
        std::uint64_t suspensions;
        std::size_t next_step;
        std::size_t delay;
        std::size_t external;
        std::size_t active;
        std::size_t writes;
        std::int32_t value;
    };
    const auto check_step = [&](ExpectedStep expected) {
        const auto time = (*scene)->simulation().clock().snapshot();
        const auto stats = runtime->scriptSystem().stats();
        assert(time.step_index == expected.step && time.elapsed == SimulationDuration{expected.step});
        assert(stats.step_invocations == 1U && stats.backend_resume_calls == expected.resumes);
        assert(stats.suspensions_admitted == expected.suspensions);
        assert(stats.next_step_waits == expected.next_step && stats.simulation_delay_waits == expected.delay);
        assert(stats.external_completion_queue_depth == expected.external && stats.resume_queue_depth == 0U);
        assert(stats.active_continuations == expected.active && stats.active_awaitables == expected.active);
        // The eager custom provider is external; subsequent built-in NextStep and seconds are owner-local.
        assert(stats.completion_capability_constructions == 1U);
        assert(runtime->scriptSystem().failures().empty());
        assert(g_probe_system != nullptr && g_probe_system->async_starts == 1U);
        assert(g_probe_system->reads == 0U && g_probe_system->writes == expected.writes);
        assert(g_probe_system->value == expected.value && g_total_writes == expected.writes);
        std::printf("LUA_STEP,step=%llu,resumes=%llu,suspensions=%llu,next=%zu,delay=%zu,external=%zu,"
            "active=%zu,writes=%zu,value=%d\n", time.step_index, stats.backend_resume_calls,
            stats.suspensions_admitted, stats.next_step_waits, stats.simulation_delay_waits,
            stats.external_completion_queue_depth, stats.active_continuations, g_total_writes, g_probe_system->value);
    };
#endif

    assert((*scene)->simulation().execute(*executor, SimulationDuration{1}));
    assert(runtime->scriptSystem().activeContinuationCount() == 1U);
    assert(runtime->scriptSystem().failures().empty());
#if defined(LUX_LUA_PORTABILITY_ARTIFACT)
    check_step({1U, 0U, 1U, 0U, 0U, 1U, 1U, 1U, 1});
#endif
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
    // The eager post is outside step 1's captured frontier. Step 2 admits it and registers NextStep;
    // step 3 resumes and registers the packaged script's 2 ns delay at elapsed=3, deadline=5.
    // Repeating the Scene stable boundary must not advance this sequence or capture a larger frontier.
    check_step({1U, 0U, 1U, 0U, 0U, 1U, 1U, 1U, 1});
    constexpr std::array expected_steps{
        ExpectedStep{2U, 1U, 2U, 1U, 0U, 0U, 1U, 1U, 1},
        ExpectedStep{3U, 2U, 3U, 0U, 1U, 0U, 1U, 1U, 1},
        ExpectedStep{4U, 2U, 3U, 0U, 1U, 0U, 1U, 1U, 1},
        ExpectedStep{5U, 3U, 3U, 0U, 0U, 0U, 0U, 2U, 1234}
    };
    for (const auto expected : expected_steps)
    {
        assert((*scene)->simulation().execute(*executor, SimulationDuration{1}));
        check_step(expected);
        assert((*scene)->executeStablePoint());
        check_step(expected);
    }
#else
    assert((*scene)->simulation().execute(*executor, SimulationDuration{1}));
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
    testDeferredScene(*meta, timer, *executor);
    execution->requestStop();
    assert(execution->join());
    meta::ReflectionRegistry::destroyRegistry();
    return 0;
}

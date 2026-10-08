#pragma once
#include <lux/engine/scene/ScriptRuntimeSystem.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/scene/ScenePackage.hpp>
#include <lux/engine/scene/SceneSystemInstaller.hpp>
#include <lux/engine/scene/scripting/AssetAbilityLua.hpp>
#include <lux/engine/scene/scripting/SkeletonAbility.hpp>
#include <lux/engine/simulation/SimulationSystemInstaller.hpp>
#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>
#include <lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp>
#include <lux/engine/resource/asset/animation/SkeletonAsset.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>
#include <DelayAbility.ability.generated.hpp>
#include <DelayAbility.ability.lua.generated.hpp>



#include <cassert>
#include <cstdio>
#include <filesystem>
#include <source_location>
#include <thread>

namespace
{
    using namespace lux;
    using namespace lux::simulation;
    using namespace lux::simulation::script;
    namespace native = lux::scene::script;

    template <class T> auto take(T value, std::source_location where = std::source_location::current())
    {
        if (!value)
            std::fprintf(stderr, "EC2 scene failure %s:%u\n", where.file_name(), where.line());
        assert(value);
        return std::move(*value);
    }

    struct Probe final
    {
        inline static constexpr std::string_view WorldTypes[]{"*"};
        inline static constexpr auto Access = makeSystemAccessSpec<TComponentWrite<std::int32_t>>();
        inline static constexpr std::array Hooks{makeHookPointSpec<void()>({1}, "tick", true, true)};
        inline static constexpr SimulationSystemDescription Description{
            {.canonical_name = "lux.ec2.AssetSceneProbe", .version = 1, .supported_world_types = WorldTypes}, Hooks
        };
        THookPoint<void()> hook;
        TScriptHookEndpoint<void()> endpoint;
        bool started{};

        Probe(ecs::Registry& registry, system::SystemInstanceId id) : endpoint(id, {1}, hook)
        {
            // This fixture's native Simulation creates its transient gameplay object.
            // The persisted author package remains empty and must remain unchanged after Play.
            const auto entity = registry.create();
            assert(entity == ecs::Entity{0});
            registry.emplace<std::int32_t>(entity, 7);
            assert(hook.prepare(1) == EEndpointMutationError::NONE);
        }
    };

    SimulationSystemRegistration probeRegistration()
    {
        return {
            .type = system::systemTypeId(Probe::Description.type.canonical_name),
            .cpp_type = cxx::typeToken<Probe>(), .description = &Probe::Description, .access = Probe::Access.spec(),
            .install = [](SimulationSystemInstaller& installer, SimulationSystemView description) noexcept
                -> cxx::expected<void, SimulationSystemBuildFailure> {
                auto probe = installer.emplaceSystem<Probe>(description.instanceId(), installer.registry(),
                    description.instanceId());
                if (!probe) return cxx::unexpected(probe.error());
                auto published = installer.publishScriptHook(description.instanceId(), (*probe)->endpoint.descriptor());
                if (!published) return published;
                auto task = installer.addSystemTask<Probe>(description.instanceId(), [](Probe&) noexcept {});
                if (!task) return task;
                return installer.addSystemHookTask<Probe>(description.instanceId(), {1},
                    [](Probe& probe, const HookInvocation& invocation) noexcept {
                        if (!probe.started && probe.hook.handlerCount() != 0)
                        {
                            assert(probe.hook.dispatch(invocation) == 1);
                            probe.started = true;
                        }
                    });
            }
        };
    }

    const asset::AssetId ScriptId{*uuids::uuid::from_string("12345678-1111-2222-3333-123456789abc")};
    const asset::AssetId SkeletonId{*uuids::uuid::from_string("fedcba98-7654-4abc-ffff-ffffffffffff")};
    const asset::AssetId SceneId{*uuids::uuid::from_string("12345678-2222-3333-4444-123456789abc")};
    const world::WorldObjectId ObjectId{*uuids::uuid::from_string("12345678-3333-4444-5555-123456789abc")};

    struct ScriptObservation final { const lux::scene::ScriptRuntimeSystem* runtime; };

    lux::scene::SceneSystemRegistration observedScriptRegistration()
    {
        auto registration = lux::scene::builtinScriptRuntimeSystemRegistration();
        registration.install = [](lux::scene::SceneSystemInstaller& installer,
            lux::scene::SceneSystemDescription description) noexcept {
            auto installed = lux::scene::builtinScriptRuntimeSystemRegistration().install(installer, description);
            if (installed)
                installer.registry().ctx().emplace<ScriptObservation>(
                    installer.findSystem<lux::scene::ScriptRuntimeSystem>(description.instanceId())
                );
            return installed;
        };
        return registration;
    }

    struct Fixture final
    {
        process::ExecutionRuntime execution{take(process::ExecutionRuntime::create(
            {1, 64, 64, {32}, process::BlockingSchedulerConfig{1, 64}}
        ))};
        process::TaskScope files{execution};
        asset::AssetVfs vfs;
        asset::MountLease mount;
        std::shared_ptr<process::asset_loading::VfsAssetReadEndpoint> endpoint;
        std::shared_ptr<const lux::script::ScriptArtifactAsset> artifact;
        const std::array<lux::script::lua::ScriptAbilityLuaContribution, 3> contributions{
            native::assetAbilityLua(), native::skeletonAbilityLua(),
            lux::script::lua::makeScriptAbilityLuaContribution<DelayAbility>()
        };
        const ScriptHostComponentContract component{
            123, semantic::typeId(semantic::TTypeTraits<std::int32_t>::CanonicalName),
            semantic::TTypeTraits<std::int32_t>::CanonicalName,
            semantic::TTypeTraits<std::int32_t>::AbiKind, sizeof(std::int32_t), alignof(std::int32_t)
        };
        const std::array<LuaComponentBinding, 1> lua_components{{"Counter", component.component_type,
            component.semantic_type, std::string{component.canonical_name}, component.abi_kind,
            component.size, component.alignment}};
        const std::array<ScriptDeferredComponent, 1> components{scriptDeferredComponent<std::int32_t>(component)};
        const std::array<LuaPreparedBlockClass, 1> blocks{{23, 2}};
        LuaScriptBackend backend{take(LuaScriptBackend::create(LuaScriptBackendConfig{
            .instance_capacity = 2, .prepared_call_capacity = 4, .continuation_capacity = 4,
            .execution_depth_capacity = 4, .ability_catalog_method_capacity = 23, .prepared_ability_capacity = 46,
            .components = lua_components, .abilities = contributions,
            .prepared_ability_blocks = blocks, .prepared_ability_storage_bytes = 65536
        }))};
        const std::array<ScriptBackendDescriptor, 1> backends{backend.descriptor()};
        const std::array<lux::scene::ScriptRuntimeHost::AssetCapabilityFactory, 1> factories{
            &native::publishSkeletonAbility
        };
        std::shared_ptr<lux::scene::ScriptRuntimeHost> host;
        std::shared_ptr<SimulationSystemRegistry> systems{std::make_shared<SimulationSystemRegistry>()};
        const std::array<lux::scene::SceneSystemRegistration, 1> registrations{
            observedScriptRegistration()
        };
        ecs::ComponentSchemaSet schemas{take(ecs::ComponentSchemaSet::build({}))};
        lux::scene::ScenePackage package;
        std::unique_ptr<lux::scene::SceneRuntime> runtime{take(lux::scene::SceneRuntime::create(execution, {0, 1024}))};

        Fixture(const std::filesystem::path& script_file, const std::filesystem::path& pak_file)
        {
            auto skeleton = std::make_shared<rdesc::Skeleton>();
            skeleton->bones = {{"root", -1, Eigen::Affine3f::Identity(), Eigen::Affine3f::Identity()},
                {"child", 0, Eigen::Affine3f::Identity(), Eigen::Affine3f::Identity()}};
            auto data = take(asset::SkeletonAsset::create({SkeletonId, asset::SkeletonAsset::asset_type}, skeleton));
            auto encoded = std::make_shared<const std::vector<std::byte>>(
                take(asset::TAssetSerDeser<asset::SkeletonAsset>::encode(*data, asset::AssetEncodeLimits{8192}))
            );
            const auto bytes = cxx::SharedBytes<>::fromOwner(encoded, std::span<const std::byte>{*encoded});
            std::string publication_error;
            const bool published = asset::writePakFile(pak_file, {
                {ScriptId, lux::script::ScriptArtifactPrimaryMagic, "scripts/assets", script_file},
                {SkeletonId, asset::SkeletonAsset::primary_magic, "models/rig", {}, bytes}
            }, "/Game", &publication_error);
            if (!published)
                std::fprintf(stderr, "Pak publication failed: %s\n", publication_error.c_str());
            assert(published);
            const auto provider = take(asset::PakAssetProvider::loadFromFile(pak_file));
            mount = take(vfs.mount({"/Game", provider}));
            assert(vfs.view().resolve("/Game/scripts/assets") == ScriptId);
            endpoint = take(process::asset_loading::VfsAssetReadEndpoint::create(
                vfs.view().capture(), take(execution.blocking()), files, {4}
            ));
            auto loading = take(files.submit({"Load runtime script package", "EC2"},
                [&](process::TaskReporter report) noexcept {
                    auto read = process::asset_loading::loadAsset<lux::script::ScriptArtifactAsset>(
                        endpoint->port(), execution.cpu(), ScriptId, {65536, 262144, 8}, report.stopToken()
                    );
                    using Value = cxx::expected<std::shared_ptr<const lux::script::ScriptArtifactAsset>,
                        process::asset_loading::AssetLoadFailure>;
                    auto value = stdexec::then(std::move(read), [](auto asset) noexcept { return Value{std::move(asset)}; });
                    return stdexec::upon_error(std::move(value), [](auto failure) noexcept {
                        return Value{cxx::unexpected{std::move(failure)}};
                    });
                }, [&](process::TTaskResult<std::shared_ptr<const lux::script::ScriptArtifactAsset>,
                    process::asset_loading::AssetLoadFailure>&& result) noexcept {
                    assert(result);
                    artifact = std::move(*result);
                }));
            assert(execution.waitUntil([&]() noexcept { return artifact != nullptr; }));
            assert(artifact->data().description().api_requirements.size() == 3);
            assert(systems->add(probeRegistration()));
            const auto base = [] {
                SimulationDescriptionBuilder builder;
                assert(builder.addSystem({1}, "game", Probe::Description));
                assert(builder.addExecutionDependency(SimulationExecutionPoint::task({1}),
                    SimulationExecutionPoint::hook({1}, {1})));
                return builder;
            };
            const auto description = take(base().build());
            native::ScriptSystemDescriptionBuilder scripts;
            assert(scripts.addMount({{1}, ScriptId, native::EntityScriptMount{ObjectId}, true,
                {{1, HookScriptTarget{{1}, {1}}}}}));
            const auto mounts = take(std::move(scripts).build(description));
            auto builder = base();
            assert(native::addScriptSystemData(builder, mounts, {65536, 262144, 65536}));
            const auto simulation = std::make_shared<const SimulationDescription>(take(std::move(builder).build()));
            lux::scene::SceneDescriptionBuilder scene;
            assert(scene.addSystem({1}, "scripts", registrations[0].type, 1, {}, 0));
            assert(scene.bindRequirement({1}, "script_runtime_host", "script-runtime"));
            assert(scene.bindRequirement({1}, "timer", "timer"));
            package = take(lux::scene::createScenePackage(SceneId, "EC2 runtime", {}, simulation,
                take(std::move(scene).buildResolved())));
            host = std::make_shared<lux::scene::ScriptRuntimeHost>(lux::scene::ScriptRuntimeHost{
                .execution = execution,
                .limits = {8, 1, 4, 4, 4, 4, 256, 4, 4, 4, 4, 4},
                .codec_limits = {65536, 262144, 65536}, .real_delay_capacity = 4,
                .artifacts = {this, [](void* context, const asset::AssetId& id, ResolvedScriptArtifact& result) noexcept {
                    const auto& fixture = *static_cast<Fixture*>(context);
                    if (id != ScriptId) return false;
                    result = {&fixture.artifact->data(), context, [](void*) noexcept {}};
                    return true;
                }},
                .world = {nullptr, [](void*, const world::WorldObjectId& id, ecs::Entity& result) noexcept {
                    if (id != ObjectId) return false;
                    result = ecs::Entity{0};
                    return true;
                }},
                .backends = backends, .components = components, .command_capacity = {16, 4096},
                .assets = endpoint->port(), .asset_limits = {1, 2, 49152, {8192, 16384, 8}},
                .asset_capabilities = factories
            });
        }

        void collect()
        {
            assert(execution.collectCompletions());
            assert(execution.dispatchTaskEvents());
        }
        void frame()
        {
            collect();
            const auto result = take(runtime->driveFrame());
            assert(result.empty());
        }
        std::int32_t value(lux::scene::SceneInstanceId id)
        {
            auto registry = take(runtime->borrowInstance(id));
            return registry.get().get<std::int32_t>(ecs::Entity{0});
        }
        void verifyFinished(lux::scene::SceneInstanceId id)
        {
            const auto& registry = take(runtime->borrowInstance(id)).get();
            const auto& script = *registry.ctx().get<ScriptObservation>().runtime;
            assert(script.scriptSystem().failures().empty());
            assert(script.scriptSystem().activeContinuationCount() == 0);
            assert(script.scriptSystem().activeAwaitableCount() == 0);
            assert(script.commandStats().accepted == 1 && script.commandStats().rejected == 0);
        }
    };
}


#include <lux/engine/simulation/ScriptRuntimeInput.hpp>
#include <optional>
#include "Behavior.hpp"
#include "Behavior.InstalledBehavior.script.generated.hpp"
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/scripting/cpp_static/CppStaticScriptBridge.hpp>
#include <lux/engine/task/TaskExecutor.hpp>

#include <array>
#include <iostream>
#include <string_view>

namespace
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;
    constexpr lux::system::SystemInstanceId System{501U};
    constexpr HookPointId Tick{502U};
    std::int32_t input{3};

    struct Host final
    {
        inline static constexpr auto Access = makeSystemAccessSpec<>();
        inline static constexpr std::array Hooks{
            makeHookPointSpec<void(const std::int32_t&)>(Tick, "tick", true, true)
        };
        inline static constexpr SimulationSystemDescription Description{
            .type = {.canonical_name = "installed.GeneratedScriptHost", .version = 1U}, .hooks = Hooks
        };
        Host() noexcept : endpoint(System, Tick, hook) {}
        HookPoint<void(const std::int32_t&)> hook;
        ScriptHookEndpoint<void(const std::int32_t&)> endpoint;
    };

    lux::cxx::expected<void, SimulationSystemBuildFailure> install(
        SimulationBuilder& builder, SimulationSystemView view) noexcept
    {
        auto object = builder.emplaceSystem<Host>(view.instanceId());
        if (!object) return lux::cxx::unexpected(object.error());
        if ((*object)->hook.prepare(2U) != EEndpointMutationError::NONE)
            return lux::cxx::unexpected(SimulationSystemBuildFailure{
                ESimulationSystemBuildError::CONSTRUCTION_FAILURE, view.instanceId()});
        auto result = builder.publishScriptHook(view.instanceId(), (*object)->endpoint.descriptor());
        if (!result) return result;
        result = builder.addSystemTask<Host>(view.instanceId(), [](Host&) noexcept {});
        if (!result) return result;
        return builder.addSystemHookTask<Host>(view.instanceId(), Tick,
            [](Host& host, const HookInvocation& invocation) noexcept {
                static_cast<void>(host.hook.dispatch(invocation, input));
            });
    }

    struct Source final
    {
        const lux::script::ScriptArtifact* artifact{};
        lux::asset::AssetId asset;

        std::array<ecs::Entity, 2U> entities{ecs::NullEntity, ecs::NullEntity};
        static bool resolveArtifact(void* opaque, const lux::asset::AssetId& asset, ResolvedScriptArtifact& result) noexcept
        {
            const auto& self = *static_cast<Source*>(opaque);
            if (asset != self.asset) return false;
            result.artifact = self.artifact;
            return true;
        }

    };
}

int main(int argc, char** argv)
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;
    const auto& contract = generated::InstalledBehavior;
    const std::string_view scenario = argc > 1 ? argv[1] : "normal";
    if (!contract.object.requires_host || contract.object.attach == nullptr) return 20;
    auto description = materializeCppStaticScript(contract);
    if (!description) return 1;
    auto artifact = lux::script::ScriptArtifact::create(std::move(*description), {});
    if (!artifact) return 2;
    SimulationDescriptionBuilder builder;
    if (!builder.addSystem(System, "host", Host::Description) ||
        !builder.addExecutionDependency(SimulationExecutionPoint::task(System), SimulationExecutionPoint::hook(System, Tick)))
        return 3;
    auto simulation_description = std::move(builder).build();
    if (!simulation_description) return 4;
    SimulationSystemRegistry registrations;
    const SimulationSystemRegistration registration{
        .type = lux::system::systemTypeId(Host::Description.type.canonical_name),
        .cpp_type = lux::cxx::typeToken<Host>(), .description = &Host::Description,
        .access = Host::Access.spec(), .configuration = {}, .install = &install
    };
    if (!registrations.add(registration)) return 5;
    ecs::Registry registry;
    auto simulation = Simulation::create(registry,
        std::make_shared<SimulationDescription>(std::move(*simulation_description)), registrations);
    auto executor = lux::task::TaskExecutor::create({2U, 16U});
    if (!simulation || !executor) return 6;
    const std::array pools{CppStaticScriptPoolDescription{&contract, 2U, 2U, 16384U,
        alignof(std::max_align_t), scenario == "prepare-failure" ? 1U : 8U, 512U}};
    auto backend = CppStaticScriptBackend::create(pools);
    if (!backend) return 7;
    std::array<std::uint8_t, 16U> bytes{};
    bytes[0] = 0xD1U;
    Source source;
    source.artifact = &*artifact;
    source.asset = lux::asset::AssetId{bytes};
    for (std::size_t index{}; index < source.entities.size(); ++index)
    {
        bytes[1] = static_cast<std::uint8_t>(index + 1U);
        source.entities[index] = registry.create();
    }
    std::vector<ScriptRuntimeMount> mounts;
    for (std::size_t index{}; index < source.entities.size(); ++index)
        mounts.push_back({ScriptMountId{index + 1U}, source.asset, EntityScriptScope{source.entities[index]},
                {{2U, HookScriptTarget{System, Tick}}, {3U, HookScriptTarget{System, Tick}}}});
    auto mount_description = std::optional{std::move(mounts)};
    if (!mount_description) return 9;
    auto backend_description = backend->descriptor();
    if (scenario == "host-failure")
    {
        ScriptBackendInstance rejected;
        const ScriptInstanceCreateContext missing{source.asset, EntityScriptScope{source.entities[0]}, nullptr, {1U, 1U}};
        const auto status = backend_description.createInstance(backend_description.context, missing, *artifact, rejected);
        if (status != EScriptBackendResult::HOST_CONTEXT_MISMATCH || rejected || installed_generated::begins != 0U ||
            installed_generated::constructs != 1U || installed_generated::destroys != 1U) return 21;
        ScriptBehavior unattached;
        auto invalid = missing;
        invalid.behavior = &unattached;
        if (backend_description.createInstance(backend_description.context, invalid, *artifact, rejected) !=
                EScriptBackendResult::HOST_CONTEXT_MISMATCH || rejected || installed_generated::begins != 0U ||
            installed_generated::constructs != 2U || installed_generated::destroys != 2U) return 29;
        return 0;
    }
    installed_generated::fail_construction = scenario == "construct-failure";
    auto system = ScriptSystem::create(
        simulation->description(),
        *planScriptRuntimeCapacity(*mount_description),
        *mount_description,
        registry,
        simulation->clock(),
        {8U, 2U, 8U, 8U, 8U, 8U, 64U, 8U, 8U, 8U, 8U, 8U},
        {&source, &Source::resolveArtifact},
        simulation->scriptApiCapabilities(),
        std::span{&backend_description, 1U},
        simulation->scriptHookEndpoints(),
        simulation->scriptEventEndpoints()
    );
    if (!system) return 10;
    const auto prepared = system->prepare();
    if (scenario == "prepare-failure" || scenario == "construct-failure")
    {
        if (prepared || installed_generated::begins != 0U ||
            installed_generated::constructs != installed_generated::destroys ||
            installed_generated::observed != 0) return 22;
        if (scenario == "prepare-failure" && installed_generated::observations[0].attaches != 1U) return 23;
        return system->shutdown() ? 0 : 24;
    }
    if (!prepared) return 10;
    auto connection = simulation->bindHookCallbacks({&*system,
        [](void* state, const SimulationClockSnapshot&, bool stable) noexcept {
            auto& runtime = *static_cast<ScriptSystem*>(state);
            if (stable) runtime.beginStableAdmission();
            return static_cast<bool>(runtime.processLifecycle());
        },
        [](void* state, const SimulationClockSnapshot&, bool stable) noexcept {
            return !stable || static_cast<bool>(static_cast<ScriptSystem*>(state)->executeStablePoint());
        },
        [](void* state, const SimulationClockSnapshot&) noexcept {
            return static_cast<bool>(static_cast<ScriptSystem*>(state)->processLifecycle());
        }});
    if (!connection || !simulation->execute(*executor, SimulationDuration{1}) || installed_generated::observed != 14)
        return 11;
    input = 7;
    if (!simulation->execute(*executor, SimulationDuration{1}) || installed_generated::observed != 18) return 12;
    if (installed_generated::begins != 2U || system->stats().active_continuations != 0U) return 13;
    if (!simulation->execute(*executor, SimulationDuration{1}) || system->stats().active_continuations != 2U) return 14;
    const auto old_entity = source.entities[0];
    registry.destroy(old_entity);
    source.entities[0] = ecs::NullEntity;
    if (!simulation->execute(*executor, SimulationDuration{1})) return 15;
    if (installed_generated::ends != 1U || installed_generated::destroys != 1U ||
        backend->stats().active_frames != 0U || system->stats().active_continuations != 0U) return 16;
    source.entities[0] = registry.create();
    std::array<ScriptMountStatus, 2U> changes;
    if (!system->collectMountStatusChanges(changes)) return 31;
    (*mount_description)[0].scope = EntityScriptScope{source.entities[0]};
    if (!system->mountResolvedBatch(std::span{&(*mount_description)[0], 1U})) return 32;
    if (entt::to_entity(source.entities[0]) != entt::to_entity(old_entity)) return 30;
    if (source.entities[0] == old_entity || !simulation->execute(*executor, SimulationDuration{1})) return 25;
    if (installed_generated::begins != 3U || installed_generated::observations[0].self != old_entity ||
        installed_generated::observations[1].self != source.entities[1] ||
        installed_generated::observations[2].self != source.entities[0]) return 26;
    connection->reset();
    if (!system->shutdown()) return 17;
    if (installed_generated::attach_errors != 0U || installed_generated::ends != 3U ||
        installed_generated::destroys != 3U || backend->stats().active_frames != 0U) return 27;
    for (std::size_t index{}; index < 3U; ++index)
    {
        const auto& observation = installed_generated::observations[index];
        if (observation.attaches != 1U || observation.begins != 1U || observation.ends != 1U ||
            observation.destroys != 1U || observation.calls == 0U) return 28;
    }
    std::cout << "generated attach -> two objects -> caller Hook -> coroutine -> rematerialize -> EndPlay: PASS\n";
}

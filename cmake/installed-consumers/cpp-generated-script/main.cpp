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
        lux::world::WorldObjectId object;
        ecs::Entity entity{ecs::NullEntity};
        static bool resolveArtifact(void* opaque, const lux::asset::AssetId& asset, ResolvedScriptArtifact& result) noexcept
        {
            const auto& self = *static_cast<Source*>(opaque);
            if (asset != self.asset) return false;
            result.artifact = self.artifact;
            return true;
        }
        static bool resolveObject(void* opaque, const lux::world::WorldObjectId& object, ecs::Entity& result) noexcept
        {
            const auto& self = *static_cast<Source*>(opaque);
            if (object != self.object || self.entity == ecs::NullEntity) return false;
            result = self.entity;
            return true;
        }
    };
}

int main()
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;
    const auto& contract = generated::InstalledBehavior;
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
    const std::array pools{CppStaticScriptPoolDescription{&contract, 1U, 1U, 8192U,
        alignof(std::max_align_t), 4U, 512U}};
    auto backend = CppStaticScriptBackend::create(pools);
    if (!backend) return 7;
    std::array<std::uint8_t, 16U> bytes{};
    bytes[0] = 0xD1U;
    Source source{&*artifact, lux::asset::AssetId{bytes}, lux::world::WorldObjectId{uuids::uuid{bytes}}, registry.create()};
    ScriptSystemDescriptionBuilder mounts;
    if (!mounts.addMount({ScriptMountId{1U}, source.asset, EntityScriptMount{source.object}, true,
            {{2U, HookScriptTarget{System, Tick}}, {3U, HookScriptTarget{System, Tick}}}})) return 8;
    auto mount_description = std::move(mounts).build(simulation->description());
    if (!mount_description) return 9;
    auto backend_description = backend->descriptor();
    auto system = ScriptSystem::create(simulation->description(), *mount_description, registry, simulation->clock(),
        {8U, 1U, 8U, 8U, 8U, 8U, 64U, 8U, 8U, 8U, 8U, 8U},
        {&source, &Source::resolveArtifact}, {&source, &Source::resolveObject}, simulation->scriptApiCapabilities(),
        std::span{&backend_description, 1U}, simulation->scriptHookEndpoints(), simulation->scriptEventEndpoints());
    if (!system || !system->prepare()) return 10;
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
    if (installed_generated::begins != 1U || system->stats().active_continuations != 0U) return 13;
    if (!simulation->execute(*executor, SimulationDuration{1}) || system->stats().active_continuations != 1U) return 14;
    registry.destroy(source.entity);
    source.entity = ecs::NullEntity;
    if (!simulation->execute(*executor, SimulationDuration{1})) return 15;
    if (installed_generated::ends != 1U || installed_generated::destroys != 1U ||
        backend->stats().active_frames != 0U || system->stats().active_continuations != 0U) return 16;
    connection->reset();
    if (!system->shutdown()) return 17;
    std::cout << "generated source -> artifact -> caller Hook -> owned coroutine -> EndPlay: PASS\n";
}

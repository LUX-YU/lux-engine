#include "ConsumerBehavior.hpp"

#include <lux/engine/function/script/artifact/ScriptArtifact.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/physics2d/Physics2DSystem.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/abilities/DelayAbility.hpp>
#include <lux/engine/simulation/scripting/ScriptEventSource.hpp>
#include <lux/engine/simulation/scripting/cpp_static/CppStaticScriptBridge.hpp>
#include <lux/engine/task/TaskExecutor.hpp>

#include <array>
#include <cstdint>
#include <memory>

namespace
{
    using namespace lux;
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    inline constexpr system::SystemInstanceId PhysicsId{71U};
    inline constexpr system::SystemInstanceId ProbeId{72U};
    inline constexpr HookPointId TickHook{73U};
    inline constexpr EventPointId PulseEvent{74U};
    inline constexpr lux::script::ScriptSymbolId RunSymbol{75U};

    struct ProbeSystem final
    {
        inline static constexpr auto Access = makeSystemAccessSpec<>();
        inline static constexpr std::array Hooks{makeHookPointSpec<void()>(TickHook, "tick", true, true)};
        inline static constexpr std::array Events{makeEventPointSpec<std::int32_t>(
            PulseEvent,
            "pulse",
            TickHook,
            EEventRoute::SIMULATION_BROADCAST,
            "lux.i32",
            1U
        )};
        inline static constexpr SimulationSystemDescription Description{
            .type = {.canonical_name = "installed.CoroutineProbe", .version = 1U},
            .hooks = Hooks,
            .events = Events
        };

        using Channel = HookChannel<SimulationBroadcastRoute, std::int32_t>;
        explicit ProbeSystem(Channel& channel) noexcept
            : event(channel), hook_endpoint(ProbeId, TickHook, hook), event_endpoint(ProbeId, PulseEvent, event)
        {
            ready = hook.prepare(1U) == EEndpointMutationError::NONE;
        }

        HookPoint<void()> hook;
        Channel& event;
        Channel::Producer event_writer;
        ScriptHookEndpoint<void()> hook_endpoint;
        ScriptEventEndpoint<SimulationBroadcastRoute, std::int32_t> event_endpoint;
        bool ready{};
        bool tick_enabled{true};
        bool emit{};
    };

    ProbeSystem* ActiveProbe{};

    lux::cxx::expected<void, SimulationSystemBuildFailure> installProbe(
        SimulationBuilder& builder,
        SimulationSystemView description
    ) noexcept
    {
        auto channel = builder.createHookChannel<SimulationBroadcastRoute, std::int32_t>(
            description.instanceId(), PulseEvent, {1U, 4U});
        if (!channel)
            return lux::cxx::unexpected(channel.error());
        auto created = builder.emplaceSystem<ProbeSystem>(description.instanceId(), **channel);
        if (!created)
            return lux::cxx::unexpected(created.error());
        if (!(*created)->ready)
        {
            return lux::cxx::unexpected(SimulationSystemBuildFailure{
                ESimulationSystemBuildError::CONSTRUCTION_FAILURE,
                description.instanceId()
            });
        }
        ActiveProbe = *created;
        auto writer = builder.bindHookChannelProducer(description.instanceId(), PrimarySimulationTask, **channel);
        if (!writer)
            return lux::cxx::unexpected(writer.error());
        (*created)->event_writer = *writer;
        auto published = builder.publishScriptHook(description.instanceId(), (*created)->hook_endpoint.descriptor());
        if (!published)
            return published;
        published = builder.publishScriptEvent(description.instanceId(), (*created)->event_endpoint.descriptor());
        if (!published)
            return published;
        published = builder.addSystemTask<ProbeSystem>(description.instanceId(), [](ProbeSystem& value) noexcept {
            if (!value.emit)
                return true;
            value.emit = false;
            auto writer = value.event_writer.begin();
            return writer.record(7);
        });
        if (!published)
            return published;
        return builder.addSystemHookTask<ProbeSystem>(description.instanceId(), TickHook,
            [](ProbeSystem& value, const HookInvocation& invocation) noexcept {
                if (value.tick_enabled)
                    static_cast<void>(value.hook.dispatch(invocation));
            });
    }

    SimulationSystemRegistration probeRegistration() noexcept
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

    struct Source final
    {
        const lux::script::ScriptArtifact* artifact{};
        lux::asset::AssetId asset;
        lux::world::WorldObjectId object;
        ecs::Entity entity{ecs::NullEntity};

        static bool resolveArtifact(
            void* context,
            const lux::asset::AssetId& requested,
            ResolvedScriptArtifact& output
        ) noexcept
        {
            const auto& self = *static_cast<Source*>(context);
            if (requested != self.asset)
                return false;
            output.artifact = self.artifact;
            return true;
        }

        static bool resolveWorld(void* context, const lux::world::WorldObjectId& object, ecs::Entity& output) noexcept
        {
            const auto& self = *static_cast<Source*>(context);
            if (object != self.object)
                return false;
            output = self.entity;
            return true;
        }
    };

    lux::asset::AssetId assetId() noexcept
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes.front() = 0xC6U;
        return lux::asset::AssetId{bytes};
    }

    lux::world::WorldObjectId objectId() noexcept
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes.front() = 0xC7U;
        return lux::world::WorldObjectId{uuids::uuid{bytes}};
    }
}

int main()
{
    using namespace lux;
    using namespace lux::physics2d;
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    meta::ReflectionRegistry::initRegistry();
    Physics2DSystemConfiguration physics_configuration;
    physics_configuration.gravity_y = 0.0;
    physics_configuration.body_capacity = 8U;
    const auto encoded_physics = makePhysics2DSystemConfiguration(physics_configuration);
    if (!encoded_physics)
        return 1;
    SimulationDescriptionBuilder description_builder;
    if (!description_builder.addSystem(PhysicsId, "physics", physics2DSystemDescription(), *encoded_physics) ||
        !description_builder.addSystem(ProbeId, "probe", ProbeSystem::Description))
    {
        return 2;
    }
    for (auto producer : {PhysicsId, ProbeId})
    {
        if (!description_builder.addExecutionDependency(SimulationExecutionPoint::task(producer),
                SimulationExecutionPoint::hook(ProbeId, TickHook)))
            return 2;
    }
    if (!description_builder.addChannelProducer({ProbeId, PulseEvent, ProbeId, PrimarySimulationTask}))
        return 2;
    auto description = std::move(description_builder).build();
    if (!description)
        return 3;
    SimulationSystemRegistry registrations;
    if (!registrations.add(physics2DSystemRegistrations()) || !registrations.add(probeRegistration()))
        return 4;
    ecs::Registry registry;
    const auto collider = registry.create();
    registry.emplace<ecs::Transform2D>(collider);
    registry.emplace<BoxCollider2D>(collider);
    auto simulation = Simulation::create(
        registry,
        std::make_shared<SimulationDescription>(std::move(*description)),
        registrations
    );
    auto executor = task::TaskExecutor::create({4U, 16U});
    if (!simulation || !executor || !simulation->execute(*executor, SimulationDuration{}))
        return 5;
    if (ActiveProbe == nullptr)
        return 6;

    const auto event_source = projectScriptEventSource(
        simulation->description().findEvent(ProbeId, PulseEvent),
        ActiveProbe->event_endpoint.descriptor(),
        "Gameplay",
        "pulse"
    );
    if (!event_source)
        return 7;
    auto typed_event = CppScriptEventSource<std::int32_t>::create(*event_source);
    if (!typed_event)
        return 8;
    installed_consumer::pulse_event = std::move(*typed_event);

    const auto* reflected = meta::ReflectionRegistry::instance().findClass(
        "installed_consumer::CoroutineBehavior"
    );
    if (reflected == nullptr || reflected->methods.size() != 1U)
        return 9;
    const auto coroutine = makeCppStaticCoroutineExport<&installed_consumer::CoroutineBehavior::run>(
        reflected->methods.front(),
        RunSymbol
    );
    const std::array coroutines{coroutine};
    const std::array<const meta::RefMethod*, 0U> methods{};
    const std::array<lux::script::ScriptSymbolId, 0U> symbols{};
    const std::array ability_requirements{
        lux::rdesc::ScriptApiRequirement{
            lux::script::ScriptApiContractId{
                lux::script::ScriptAbilityTraits<PhysicsQuery2D>::Description.id.name()
            },
            lux::script::ScriptAbilityTraits<PhysicsQuery2D>::Description.schema_hash
        },
        lux::rdesc::ScriptApiRequirement{
            lux::script::ScriptApiContractId{
                lux::script::ScriptAbilityTraits<DelayAbility>::Description.id.name()
            },
            lux::script::ScriptAbilityTraits<DelayAbility>::Description.schema_hash
        }
    };
    const std::array event_requirements{*event_source};
    auto projected = projectCppStaticEntityScript(
        "installed.cpp-coroutine",
        "installed-cpp-coroutine-v2",
        *reflected,
        methods,
        symbols,
        {},
        nullptr,
        {},
        coroutines,
        ability_requirements,
        event_requirements
    );
    if (!projected)
        return 10;
    auto artifact = lux::script::ScriptArtifact::create(projected->description(), {});
    if (!artifact)
        return 11;
    const std::array frame_classes{
        lux::simulation::script::detail::makeUniformStorageClass(1024U, alignof(std::max_align_t), 1024U, 2U)
    };
    const std::array pools{CppStaticScriptPoolDescription{
        std::addressof(*projected),
        1U,
        1U,
        2048U,
        alignof(std::max_align_t),
        1U,
        frame_classes
    }};
    auto backend = CppStaticScriptBackend::create(pools);
    if (!backend)
        return 12;

    Source source{std::addressof(*artifact), assetId(), objectId(), registry.create()};
    ScriptSystemDescriptionBuilder script_builder;
    if (!script_builder.addMount({
            ScriptMountId{1U},
            source.asset,
            EntityScriptMount{source.object},
            true,
            {{RunSymbol, HookScriptTarget{ProbeId, TickHook}}}
        }))
    {
        return 13;
    }
    auto script_description = std::move(script_builder).build(simulation->description());
    if (!script_description)
        return 14;
    auto backend_descriptor = backend->descriptor();
    auto system = ScriptSystem::create(
        simulation->description(),
        *script_description,
        registry,
        simulation->clock(),
        {8U, 1U, 2U, 2U, 2U, 2U, 64U, 2U, 2U, 2U, 2U, 2U},
        {std::addressof(source), &Source::resolveArtifact},
        {std::addressof(source), &Source::resolveWorld},
        simulation->scriptApiCapabilities(),
        std::span{std::addressof(backend_descriptor), 1U},
        simulation->scriptHookEndpoints(),
        simulation->scriptEventEndpoints()
    );
    if (!system || !system->prepare())
        return 15;
    auto connection = simulation->bindHookCallbacks({&*system,
        [](void* context, const SimulationClockSnapshot&, bool stable) noexcept {
            auto& runtime = *static_cast<ScriptSystem*>(context);
            if (stable)
                runtime.beginStableAdmission();
            return static_cast<bool>(runtime.processLifecycle());
        },
        [](void* context, const SimulationClockSnapshot&, bool stable) noexcept {
            return !stable || static_cast<bool>(static_cast<ScriptSystem*>(context)->executeStablePoint());
        },
        [](void* context, const SimulationClockSnapshot&) noexcept {
            return static_cast<bool>(static_cast<ScriptSystem*>(context)->processLifecycle());
        }});
    if (!connection || !simulation->execute(*executor, SimulationDuration{1}) || installed_consumer::observed != 1)
        return 16;
    ActiveProbe->emit = true;
    if (!simulation->execute(*executor, SimulationDuration{1}) || installed_consumer::observed != 8)
        return 19;
    if (!simulation->execute(*executor, SimulationDuration{1}) || installed_consumer::observed != 108)
    {
        return 20;
    }
    // Retire while suspended, reuse the Entity slot, and materialize a fresh ScriptInstance generation.
    if (!simulation->execute(*executor, SimulationDuration{1}) || system->stats().active_event_waiters != 1U)
        return 22;
    const auto old_entity = source.entity;
    registry.destroy(old_entity);
    source.entity = registry.create();
    ActiveProbe->tick_enabled = false;
    if (source.entity == old_entity || !simulation->execute(*executor, SimulationDuration{1}))
        return 23;
    if (system->stats().active_event_waiters != 0U || backend->stats().active_frames != 0U)
        return 24;
    ActiveProbe->emit = true;
    if (!simulation->execute(*executor, SimulationDuration{1}) || installed_consumer::observed != 1)
        return 25;
    ActiveProbe->tick_enabled = true;
    if (!simulation->execute(*executor, SimulationDuration{1}) || system->stats().active_event_waiters != 1U)
        return 26;
    connection->reset();
    if (!system->shutdown() || backend->stats().active_frames != 0U)
        return 21;
    return 0;
}

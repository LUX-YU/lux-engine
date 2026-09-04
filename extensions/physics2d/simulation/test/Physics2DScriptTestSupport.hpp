#pragma once

#include <lux/engine/physics2d/Physics2DSystem.hpp>
#include <lux/engine/simulation/HookPoint.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationSystemRegistry.hpp>
#include <lux/engine/simulation/scripting/ScriptEndpointBridge.hpp>
#include <lux/engine/simulation/scripting/ScriptEventSource.hpp>

#include <array>
#include <cassert>
#include <exception>
#include <memory>

namespace lux::physics2d::test
{
    inline constexpr lux::system::SystemInstanceId PhysicsSystemId{0x502D1001U};
    inline constexpr lux::system::SystemInstanceId ProbeSystemId{0x502D1002U};
    inline constexpr lux::simulation::HookPointId TickHook{0x502D1003U};
    inline constexpr lux::script::ScriptSymbolId TickSymbol{0x502D1004U};
    inline constexpr lux::simulation::EventPointId PulseEvent{0x502D1005U};
    inline constexpr lux::script::ScriptSymbolId FlowTickSymbol{0x502D2001U};

    [[nodiscard]] inline lux::script::ScriptEventSourceDescription pulseEventSource()
    {
        return {
            "PhysicsBenchmark",
            "pulse",
            ProbeSystemId.value,
            PulseEvent.value,
            lux::script::EScriptEventRoute::SIMULATION_BROADCAST,
            {
                "lux.i32",
                lux::semantic::typeId("lux.i32"),
                LUX_SCRIPT_VK_INT32,
                sizeof(std::int32_t),
                alignof(std::int32_t)
            },
            lux::semantic::typeId("lux.i32"),
            1U
        };
    }

    class ProbeSystem final
    {
    public:
        inline static constexpr auto Access = lux::simulation::makeSystemAccessSpec<>();
        inline static constexpr std::array Hooks{
            lux::simulation::makeHookPointSpec<void()>(TickHook, "physics-script-tick")};
        inline static constexpr std::array Events{
            lux::simulation::makeEventPointSpec<std::int32_t>(
                PulseEvent,
                "physics-script-pulse",
                TickHook,
                lux::simulation::EEventRoute::SIMULATION_BROADCAST,
                "lux.i32",
                1U
            )
        };
        inline static constexpr lux::simulation::SimulationSystemDescription Description{
            .type = {.canonical_name = "lux.physics2d.ScriptProbe", .version = 1U},
            .hooks = Hooks,
            .events = Events
        };

        lux::simulation::HookPoint<void()> tick;
        lux::simulation::EventPoint<lux::simulation::SimulationBroadcastRoute, std::int32_t> pulse;
        std::unique_ptr<lux::simulation::script::ScriptHookEndpoint<void()>> endpoint;
        std::unique_ptr<lux::simulation::script::ScriptEventEndpoint<
            lux::simulation::SimulationBroadcastRoute,
            std::int32_t
        >> event_endpoint;
    };

    inline ProbeSystem* ActiveProbe{};

    [[nodiscard]] inline lux::cxx::expected<void, lux::simulation::SimulationSystemBuildFailure> installProbeSystem(
        lux::simulation::SimulationBuilder& builder,
        lux::simulation::SimulationSystemView description) noexcept
    {
        auto system = builder.emplaceSystem<ProbeSystem>(description.instanceId());
        if (!system)
            return lux::cxx::unexpected(system.error());
        if ((*system)->tick.prepare(1U) != lux::simulation::EEndpointMutationError::NONE)
        {
            return lux::cxx::unexpected(lux::simulation::SimulationSystemBuildFailure{
                lux::simulation::ESimulationSystemBuildError::CONSTRUCTION_FAILURE,
                description.instanceId()});
        }
        if ((*system)->pulse.prepare(1U, 1U, 1U) != lux::simulation::EEndpointMutationError::NONE)
        {
            return lux::cxx::unexpected(lux::simulation::SimulationSystemBuildFailure{
                lux::simulation::ESimulationSystemBuildError::CONSTRUCTION_FAILURE,
                description.instanceId()
            });
        }
        (*system)->endpoint =
            std::make_unique<lux::simulation::script::ScriptHookEndpoint<void()>>(description.instanceId(),
                                                                                  TickHook,
                                                                                  (*system)->tick);
        (*system)->event_endpoint = std::make_unique<lux::simulation::script::ScriptEventEndpoint<
            lux::simulation::SimulationBroadcastRoute,
            std::int32_t
        >>(description.instanceId(), PulseEvent, (*system)->pulse);
        const auto published = builder.publishScriptHook(description.instanceId(), (*system)->endpoint->descriptor());
        if (!published)
            return lux::cxx::unexpected(published.error());
        const auto published_event =
            builder.publishScriptEvent(description.instanceId(), (*system)->event_endpoint->descriptor());
        if (!published_event)
            return lux::cxx::unexpected(published_event.error());
        const auto task = builder.addSystemTask<ProbeSystem>(description.instanceId(), [](ProbeSystem&) noexcept {});
        if (!task)
            return lux::cxx::unexpected(task.error());
        ActiveProbe = *system;
        return {};
    }

    [[nodiscard]] inline lux::simulation::SimulationSystemRegistration probeRegistration() noexcept
    {
        return {.type = lux::system::systemTypeId(ProbeSystem::Description.type.canonical_name),
                .cpp_type = lux::cxx::typeToken<ProbeSystem>(),
                .description = &ProbeSystem::Description,
                .access = ProbeSystem::Access.spec(),
                .configuration = lux::serialization::noPortableValueCodec(),
                .install = &installProbeSystem};
    }

    [[nodiscard]] inline std::shared_ptr<const lux::simulation::SimulationDescription> description()
    {
        Physics2DSystemConfiguration configuration;
        configuration.gravity_y = 0.0;
        configuration.body_capacity = 64U;
        const auto encoded = makePhysics2DSystemConfiguration(configuration);
        if (!encoded)
            std::terminate();
        lux::simulation::SimulationDescriptionBuilder builder;
        const auto physics = builder.addSystem(PhysicsSystemId, "physics2d", physics2DSystemDescription(), *encoded);
        const auto probe = builder.addSystem(ProbeSystemId, "physics-script-probe", ProbeSystem::Description);
        if (!physics || !probe)
            std::terminate();
        auto built = std::move(builder).build();
        if (!built)
            std::terminate();
        return std::make_shared<lux::simulation::SimulationDescription>(std::move(*built));
    }

    [[nodiscard]] inline lux::cxx::expected<lux::simulation::Simulation, lux::simulation::SimulationSystemBuildFailure>
    createSimulation(lux::simulation::ecs::Registry& registry) noexcept
    {
        ActiveProbe = nullptr;
        lux::simulation::SimulationSystemRegistry registrations;
        const auto physics = registrations.add(physics2DSystemRegistrations());
        if (!physics)
            return lux::cxx::unexpected(lux::simulation::SimulationSystemBuildFailure{});
        const auto probe = registrations.add(probeRegistration());
        if (!probe)
            return lux::cxx::unexpected(lux::simulation::SimulationSystemBuildFailure{});
        return lux::simulation::Simulation::create(registry, description(), registrations);
    }
}

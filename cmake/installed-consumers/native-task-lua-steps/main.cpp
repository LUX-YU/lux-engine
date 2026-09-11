#include "ConsumerBehavior.hpp"
#include "ConsumerDomain.hpp"
#include "ConsumerBehavior.CoroutineBehavior.script.generated.hpp"
#include "ProbeAbility.ability.generated.hpp"
#include "ProbeAbility.ability.lua.generated.hpp"
#include <lux/engine/simulation/scripting/cpp_static/ScriptDelayCoroutine.hpp>
#include "DelayAbility.ability.lua.generated.hpp"
#include <lux/engine/simulation/scripting/native_lua_tasks/NativeLuaTaskBackend.hpp>
#include <lux/engine/simulation/scripting/ScriptEventSource.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationBuilder.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <fstream>
#include <cassert>
#include <cstdio>
#include <limits>
namespace
{
using namespace lux;
using namespace lux::simulation;
using namespace lux::simulation::script;
using namespace installed_consumer;
    struct ProbeSystem final
    {
        inline static constexpr auto Access = makeSystemAccessSpec<>();
        inline static constexpr auto Description = ProbeDescription;

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
            return writer.record(31);
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
    const lux::script::ScriptArtifact* lua{};
    const lux::script::ScriptArtifact* native{};
    lux::asset::AssetId lua_id, native_id;
    ecs::Entity entity{ecs::NullEntity};
    std::size_t leases{};
    static bool resolveArtifact(void* opaque, const lux::asset::AssetId& id, ResolvedScriptArtifact& result) noexcept
    {
        auto& self = *static_cast<Source*>(opaque);
        if (id != self.lua_id && id != self.native_id) return false;
        ++self.leases;
        result = {id == self.lua_id ? self.lua : self.native, &self,
            [](void* ptr) noexcept { --static_cast<Source*>(ptr)->leases; }};
        return true;
    }
};
struct Provider final
{
    unsigned begins{}, finishes{}, calls{};
    int value{};
    void hit(std::int32_t input) noexcept
    {
        if (input == 1) ++begins;
        else if (input == 2) ++finishes;
        else { ++calls; value = input; }
    }
};
int run(unsigned scenario)
{
    observed = 0; starts = ends = objects = destroys = frames = 0U;
    std::ifstream input(LUX_LUA_ARTIFACT, std::ios::binary | std::ios::ate);
    assert(input);
    const auto size = input.tellg();
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    assert(input && bytes.size() >= 56U);
    std::array<std::uint8_t, 16U> id_bytes{};
    for (std::size_t i{}; i < 16U; ++i) id_bytes[i] = std::to_integer<std::uint8_t>(bytes[40U+i]);
    auto decoded = lux::asset::TAssetSerDeser<lux::script::ScriptArtifactAsset>::decode(lux::asset::AssetId{id_bytes},
        lux::cxx::SharedBytes<>::copyOf(bytes), {bytes.size(), (std::numeric_limits<std::size_t>::max)(), 0U});
    assert(decoded);
    const auto& lua = (*decoded)->data();
    auto description = installed_consumer::makeDescription();
    assert(description);
    SimulationSystemRegistry registrations;
    assert(registrations.add(probeRegistration()));
    ecs::Registry registry;
    auto simulation = Simulation::create(registry, std::make_shared<SimulationDescription>(std::move(*description)),
        registrations);
    auto executor = task::TaskExecutor::create({0U, 16U});
    assert(simulation && executor && simulation->execute(*executor, SimulationDuration{}));
    assert(ActiveProbe);
    auto source_event = projectScriptEventSource(simulation->description().findEvent(ProbeId, PulseEvent),
        ActiveProbe->event_endpoint.descriptor(), "Gameplay", "pulse");
    assert(source_event);
    const auto& contract = generated::CoroutineBehavior;
    auto typed = CppScriptEventSource<std::int32_t>::create(contract, *source_event);
    assert(typed);
    pulse_event = *typed;
    auto native_description = materializeCppStaticScript(contract);
    assert(native_description);
    auto native = lux::script::ScriptArtifact::create(std::move(*native_description), {});
    assert(native);
    id_bytes[0] ^= 0x7f;
    Source source{&lua, &*native, (*decoded)->id(), lux::asset::AssetId{id_bytes}, registry.create()};
    const std::array pools{CppStaticScriptPoolDescription{&contract, 1U, 1U, 2048U, alignof(std::max_align_t), 1U, 512U}};
    const std::array routes{NativeLuaTaskRoute{75U, 75U}};
    const std::array steps{*lua.findExport(scenario == 1U ? 79U : 78U)};
    const std::array plans{NativeLuaTaskPlan{source.lua_id, lua.contentIdentity(), source.native_id,
        native->contentIdentity(), &contract, routes, steps}};
    const std::array contributions{lux::script::lua::makeScriptAbilityLuaContribution<ProbeAbility>(),
        lux::script::lua::makeScriptAbilityLuaContribution<DelayAbility>()};
    const std::array ability_blocks{LuaPreparedBlockClass{5U, 1U}}, event_blocks{LuaPreparedBlockClass{1U, 1U}};
    auto backend = NativeLuaTaskBackend::create({.lua = {.instance_capacity=1U, .prepared_call_capacity=4U,
        .continuation_capacity=0U, .execution_depth_capacity=8U, .ability_catalog_method_capacity=5U,
        .prepared_ability_capacity=5U, .abilities=contributions, .event_catalog_capacity=1U, .prepared_event_capacity=1U,
        .events=std::span{&*source_event, 1U}, .prepared_ability_blocks=ability_blocks,
        .prepared_ability_storage_bytes=8192U, .prepared_event_blocks=event_blocks, .prepared_event_storage_bytes=2048U},
        .native_pools=pools, .plans=plans, .artifacts={&source, &Source::resolveArtifact},
        .instance_capacity=1U, .prepared_method_capacity=4U});
    assert(backend);
    const std::array mounts{ScriptRuntimeMount{ScriptMountId{1U}, source.lua_id, EntityScriptScope{source.entity},
        {{75U, HookScriptTarget{ProbeId, TickHook}}}}};
    Provider provider;
    const auto binding = lux::script::bindScriptAbility<ProbeAbility>(provider);
    const std::array capabilities{publishScriptAbility(binding)};
    const auto backend_descriptor = backend->descriptor();
    auto system = ScriptSystem::create(simulation->description(), *planScriptRuntimeCapacity(mounts), mounts,
        registry, simulation->clock(), {16U,1U,2U,2U,2U,2U,64U,2U,2U,2U,2U,2U},
        {&source, &Source::resolveArtifact}, capabilities, std::span{&backend_descriptor,1U},
        simulation->scriptHookEndpoints(), simulation->scriptEventEndpoints());
    assert(system && system->prepare() && provider.begins==1U && source.leases==2U);
    struct HookContext final
    {
        ScriptSystem& system;
        std::optional<ScriptSystem::ExecutionRegion> region;
    } hook_context{*system, {}};
    auto connection = simulation->bindHookCallbacks({&hook_context,
        [](void* context, const SimulationClockSnapshot&, bool stable) noexcept {
            auto& host = *static_cast<HookContext*>(context);
            if (host.system.isShutdown()) return true;
            if (stable) host.system.beginStableAdmission();
            const auto lifecycle = host.system.processLifecycle();
            if (!lifecycle && lifecycle.error() != EScriptSystemError::INVOCATION_FAILURE) return false;
            if (host.system.isShutdown()) return true;
            auto region = host.system.beginExecutionRegion();
            if (!region) return false;
            host.region.emplace(std::move(*region));
            return true;
        },
        [](void* context, const SimulationClockSnapshot&, bool stable) noexcept {
            auto& host = *static_cast<HookContext*>(context);
            if (!host.region) return host.system.isShutdown();
            const bool resumed = !stable || static_cast<bool>(host.system.executeStablePoint());
            if (!host.region->finish()) return false;
            host.region.reset();
            return resumed;
        },
        [](void* context, const SimulationClockSnapshot&) noexcept {
            auto& system = static_cast<HookContext*>(context)->system;
            if (system.isShutdown()) return true;
            const auto result = system.processLifecycle();
            return result || result.error() == EScriptSystemError::INVOCATION_FAILURE;
        },
        [](void* context, const SimulationClockSnapshot&) noexcept {
            auto& host = *static_cast<HookContext*>(context);
            if (host.region)
            {
                if (!host.region->finish()) std::terminate();
                host.region.reset();
            }
            if (!host.system.isShutdown())
                static_cast<void>(host.system.processLifecycle(EScriptLifecycleAdmission::RETIRE_ONLY));
        }});

    assert(connection && simulation->execute(*executor, SimulationDuration{1'000'000}));
    assert(starts==1U && ends==0U && system->stats().active_event_waiters==1U);
    ActiveProbe->tick_enabled=false;
    ActiveProbe->emit=true;
    if (scenario == 2U) registry.destroy(source.entity);
    assert(simulation->execute(*executor, SimulationDuration{1'000'000}));
    if (scenario != 2U) assert(system->stats().next_step_waits==1U && ends==0U);
    assert(simulation->execute(*executor, SimulationDuration{1'000'000}));
    if (scenario == 0U)
        assert(observed==32 && ends==1U && provider.calls==1U && provider.value==32 && system->failures().empty());
    else
        assert(observed==0 && ends==0U && provider.calls==0U);
    if (scenario == 1U) assert(!system->failures().empty());
    connection->reset();
    assert(system->shutdown());
    const auto stats=backend->stats();
    assert(stats.lua.vm_coroutine_creations==0U && stats.lua.vm_coroutine_resumes==0U);
    assert(stats.native.active_frames==0U && stats.active_instances==0U && stats.active_methods==0U);
    assert(objects==1U && destroys==1U && frames==1U && provider.finishes==1U && source.leases==0U);
    std::printf("INSTALLED_NATIVE_LUA scenario=%u starts=1 completed=%zu value=%d provider=%u begin=1 end=1 "
        "frame_destroy=1 objects=1 lua_threads=0 lua_resumes=0 leases=0\n", scenario, ends, observed, provider.calls);
    return 0;
}
}
int main()
{
    for (unsigned scenario{}; scenario<3U; ++scenario) if (run(scenario)) return 1;
}

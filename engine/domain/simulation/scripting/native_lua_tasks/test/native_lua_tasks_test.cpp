#include "../../../builtin/script/test/ScriptRuntimeTestRegion.hpp"
#include "../../../builtin/script/test/ScriptTestClock.hpp"
#include "DelayAbility.ability.lua.generated.hpp"
#include "NativeTask.Na1Task.script.generated.hpp"
#include "NativeTask.hpp"
#include "TaskDomain.hpp"
#include "TaskProbe.ability.generated.hpp"
#include "TaskProbe.ability.lua.generated.hpp"
#include <cstdio>
#include <lua.hpp>
#include <lux/engine/simulation/scripting/ScriptEventSource.hpp>
#include <lux/engine/simulation/scripting/ScriptLifecycle.hpp>
#include <lux/engine/simulation/scripting/native_lua_tasks/NativeLuaTaskBackend.hpp>

namespace
{
using namespace lux::simulation;
using namespace lux::simulation::script;
using namespace lux::simulation::script::test;
using lux::simulation::script::generated::Na1Task;
lua_State *observed_vm{};
LuxLuaTypedWorker original_probe{};
LuxLuaBoundaryOutcome observeProbe(lua_State *vm) noexcept
{
    observed_vm = vm;
    return original_probe(vm);
}
struct Probe final
{
    std::size_t begins{}, ends{}, calls{};
    void *callback_context{};
    void (*callback)(void *, std::int32_t) noexcept {};
    std::int32_t hit(std::int32_t code) noexcept
    {
        if (code == 1)
            ++begins;
        else if (code == 2)
            ++ends;
        else
            ++calls;
        if (callback)
            callback(callback_context, code);
        return code;
    }
};
lux::asset::AssetId asset(std::uint8_t value)
{
    std::array<std::uint8_t, 16U> bytes{};
    bytes[0] = value;
    return lux::asset::AssetId{bytes};
}
lux::script::ScriptArtifact nativeArtifact()
{
    auto description = materializeCppStaticScript(Na1Task);
    assert(description);
    auto result = lux::script::ScriptArtifact::create(std::move(*description), {});
    assert(result);
    return std::move(*result);
}
lux::script::ScriptArtifact luaArtifact(std::string_view body)
{
    lux::rdesc::Script description;
    description.module_name = "lux.na1.lua";
    description.exports = {{"run", 101U, {}, {}},
                           {"apply",
                            102U,
                            {lux::rdesc::makeScriptValueType<std::int32_t>()},
                            {lux::rdesc::makeScriptValueType<std::int32_t>()}},
                           {"begin_life", 103U, {}, {}},
                           {"end_life", 104U, {lux::rdesc::makeScriptValueType<EScriptEndPlayReason>()}, {}}};
    description.lifecycle = {103U, 104U};
    const auto &probe = lux::script::ScriptAbilityTraits<na1::TaskProbe>::Description;
    const auto &delay = lux::script::ScriptAbilityTraits<DelayAbility>::Description;
    description.api_requirements = {{lux::script::ScriptApiContractId{probe.id.name()}, probe.schema_hash},
                                    {lux::script::ScriptApiContractId{delay.id.name()}, delay.schema_hash}};
    description.body = lux::rdesc::LuaSourceScript{"Na1Lua", {101U}};
    const auto simulation = na1::domain();
    auto event =
        describeScriptEventSource<std::int32_t>(simulation.findEvent(na1::System, na1::Event), "Task", "event");
    assert(event);
    description.event_requirements.push_back(std::move(*event));
    const std::string source =
        "return { begin_life=function(self) assert(self.value==nil); self.value=1; lux.Ability.TaskProbe.hit(1) end, "
        "end_life=function(self,reason) assert(self.value>=1 and not self.ended); self.ended=true; "
        "lux.Ability.TaskProbe.hit(2) end, "
        "run=function(self) local p=lux.Event.Task.event(); self.value=self.value+p end, "
        "apply=function(self,p) " +
        std::string(body) + " end }";
    std::vector<std::byte> payload;
    for (const auto value : source)
        payload.push_back(static_cast<std::byte>(value));
    auto result = lux::script::ScriptArtifact::create(std::move(description), std::move(payload));
    assert(result);
    return std::move(*result);
}
struct Harness final
{
    explicit Harness(std::size_t count, std::string_view body = "self.value=self.value+p; return self.value",
                     unsigned invalid = 0U)
        : count(count), simulation(na1::domain()), lua_artifact(luaArtifact(body)), native_artifact(nativeArtifact())
    {
        na1::constructed = na1::destroyed = na1::started = na1::completed = na1::frames_destroyed = na1::unreachable =
            0U;
        na1::results.assign(count, 0);
        assert(hook.prepare(count) == EEndpointMutationError::NONE);
        assert(event.prepare({1U, 4U}) == EEndpointMutationError::NONE);
        hook_endpoint.emplace(na1::System, na1::Hook, hook);
        event_endpoint.emplace(na1::System, na1::Event, event);
        auto source = projectScriptEventSource(simulation.findEvent(na1::System, na1::Event),
                                               event_endpoint->descriptor(), "Task", "event");
        assert(source);
        event_source = std::move(*source);
        auto typed = CppScriptEventSource<std::int32_t>::create(Na1Task, event_source);
        assert(typed);
        na1::event_source = *typed;
        const std::array blocks{LuaPreparedBlockClass{1U, count}};
        const std::array ability_blocks{LuaPreparedBlockClass{5U, count}};
        auto probe_contribution = lux::script::lua::makeScriptAbilityLuaContribution<na1::TaskProbe>();
        probe_methods.assign(probe_contribution.methods.begin(), probe_contribution.methods.end());
        original_probe = probe_methods.front().entry;
        probe_methods.front().entry = &observeProbe;
        probe_contribution.methods = probe_methods;
        const std::array contributions{probe_contribution,
                                       lux::script::lua::makeScriptAbilityLuaContribution<DelayAbility>()};
        const std::array pools{CppStaticScriptPoolDescription{&Na1Task, count, count * 2U, count * 2048U,
                                                              alignof(std::max_align_t), count * 2U, 512U, true}};
        const std::array routes{NativeLuaTaskRoute{101U, 101U}};
        std::array steps{*lua_artifact.findExport(102U)};
        if (invalid == 2U)
            steps[0].returns[0] = lux::rdesc::makeScriptValueType<double>();
        const std::array plans{NativeLuaTaskPlan{
            asset(1U), invalid == 1U ? native_artifact.contentIdentity() : lua_artifact.contentIdentity(), asset(2U),
            native_artifact.contentIdentity(), &Na1Task, routes, steps}};
        auto created = NativeLuaTaskBackend::create({.lua = {.instance_capacity = count,
                                                             .prepared_call_capacity = count * 4U,
                                                             .continuation_capacity = 0U,
                                                             .execution_depth_capacity = 8U,
                                                             .ability_catalog_method_capacity = 5U,
                                                             .prepared_ability_capacity = count * 5U,
                                                             .abilities = contributions,
                                                             .event_catalog_capacity = 1U,
                                                             .prepared_event_capacity = count,
                                                             .events = std::span{&event_source, 1U},
                                                             .track_vm_allocations = true,
                                                             .prepared_ability_blocks = ability_blocks,
                                                             .prepared_ability_storage_bytes = count * 4096U,
                                                             .prepared_event_blocks = blocks,
                                                             .prepared_event_storage_bytes = count * 1024U},
                                                     .native_pools = pools,
                                                     .plans = plans,
                                                     .artifacts = {this, &resolve},
                                                     .instance_capacity = count,
                                                     .prepared_method_capacity = count * 4U});
        assert(created);
        backend.emplace(std::move(*created));
        descriptor = backend->descriptor();
        for (std::size_t i{}; i < count; ++i)
        {
            const auto entity = registry.create();
            entities.push_back(entity);
            mounts.push_back({ScriptMountId{i + 1U},
                              asset(1U),
                              EntityScriptScope{entity},
                              {{101U, HookScriptTarget{na1::System, na1::Hook}}}});
        }
        const auto capacity = planScriptRuntimeCapacity(mounts);
        assert(capacity);
        const auto endpoint = hook_endpoint->descriptor();
        const auto events = event_endpoint->descriptor();
        const auto binding = lux::script::bindScriptAbility<na1::TaskProbe>(probe);
        const std::array capabilities{publishScriptAbility(binding)};
        auto runtime = ScriptSystem::create(simulation, *capacity, mounts, registry, clock_owner.clock(),
                                            {64U, count, count * 2U, 2U, count * 2U, count * 2U, 64U, count * 2U,
                                             count * 2U, count * 2U, count * 2U, count * 2U},
                                            {this, &resolve}, capabilities, std::span{&descriptor, 1U},
                                            std::span{&endpoint, 1U}, std::span{&events, 1U});
        assert(runtime);
        system.emplace(std::move(*runtime));
    }
    static bool resolve(void *opaque, const lux::asset::AssetId &id, ResolvedScriptArtifact &output) noexcept
    {
        auto &self = *static_cast<Harness *>(opaque);
        if (id != asset(1U) && id != asset(2U))
            return false;
        ++self.leases;
        output = {id == asset(1U) ? &self.lua_artifact : &self.native_artifact, &self,
                  [](void *pointer) noexcept { --static_cast<Harness *>(pointer)->leases; }};
        return true;
    }
    void occurrence()
    {
        {
            auto writer = event.begin(0U);
            assert(writer.record(std::int32_t{31}));
        }
        assert(deliverRuntimeEvent(*system, event_endpoint) == 1U);
        assert(executeRuntimeStablePoint(*system));
    }
    void closed()
    {
        assert(system->shutdown());
        const auto core = system->stats();
        const auto child = backend->stats();
        assert(leases == 0U && child.active_instances == 0U && child.active_methods == 0U);
        assert(child.active_companion_leases == 0U && child.native.active_frames == 0U);
        assert(core.active_continuations == 0U && core.active_awaitables == 0U && core.active_event_waiters == 0U);
        assert(na1::constructed == na1::destroyed && na1::started == na1::frames_destroyed);
        assert(na1::unreachable == 0U && probe.begins == probe.ends);
        // Real backend counters, including attempts that did not reach a suspended state.
        assert(child.lua.vm_coroutine_creations == 0U && child.lua.vm_coroutine_resumes == 0U);
        assert(child.lua.vm_coroutine_releases == 0U);
    }
    std::size_t count, leases{};
    SimulationDescription simulation;
    lux::script::ScriptArtifact lua_artifact, native_artifact;
    lux::script::ScriptEventSourceDescription event_source;
    ecs::Registry registry;
    ScriptTestClock clock_owner{registry};
    Probe probe;
    std::vector<lux::script::lua::ScriptAbilityLuaMethodProjection> probe_methods;
    HookPoint<void()> hook;
    HookChannel<SimulationBroadcastRoute, std::int32_t> event;
    std::optional<ScriptHookEndpoint<void()>> hook_endpoint;
    std::optional<ScriptEventEndpoint<SimulationBroadcastRoute, std::int32_t>> event_endpoint;
    std::vector<ecs::Entity> entities;
    std::vector<ScriptRuntimeMount> mounts;
    std::optional<NativeLuaTaskBackend> backend;
    ScriptBackendDescriptor descriptor;
    std::optional<ScriptSystem> system;
};
void normal(std::size_t count, unsigned mode)
{
    na1::mode = mode;
    Harness h(count);
    assert(h.system->prepare());
    assert(h.leases == count * 2U && na1::constructed == count);
    assert(dispatchRuntimeHook(*h.system, h.hook) == 1U);
    assert(na1::started == count);
    assert(h.system->stats().active_event_waiters == count);
    const auto rounds = mode == 3U ? 32U : 1U;
    for (unsigned i{}; i < rounds; ++i)
    {
        h.occurrence();
        for (const auto result : na1::results)
            assert(result == static_cast<int>(1U + (i + 1U) * 31U));
    }
    assert(na1::completed == count && na1::frames_destroyed == count);
    assert(h.system->failures().empty());
    h.closed();
    std::printf("CASE event count=%zu rounds=%u payload=31 completed=%zu lua_threads=0 lua_resumes=0 leases=%zu\n",
                count, rounds, na1::completed, h.leases);
}
} // namespace
int main()
{
    normal(64U, 0U);
    normal(1000U, 0U);
    normal(64U, 3U);
    for (const unsigned mode : {4U, 5U})
    {
        na1::mode = mode;
        Harness h(64U);
        assert(h.system->prepare());
        assert(dispatchRuntimeHook(*h.system, h.hook) == 1U);
        for (auto value : na1::results)
            assert(value == 2);
        assert(h.system->stats().next_step_waits == 64U);
        h.clock_owner.advance(SimulationDuration{1'000'000});
        assert(executeRuntimeStablePoint(*h.system));
        if (mode == 5U)
        {
            assert(h.system->stats().active_event_waiters == 64U);
            h.occurrence();
            assert(h.system->stats().simulation_delay_waits == 64U && na1::completed == 0U);
            h.clock_owner.advance(SimulationDuration{1'000'000});
            assert(executeRuntimeStablePoint(*h.system));
        }
        assert(na1::completed == 64U);
        for (auto value : na1::results)
            assert(value == (mode == 4U ? 12 : 33));
        assert(h.system->stats().completion_capability_constructions == 0U);
        h.closed();
        std::printf("CASE timer mode=%u completed=64 begins=64 ends=64 frames=64\n", mode);
    }
    for (unsigned invalid = 1U; invalid <= 2U; ++invalid)
    {
        Harness h(1U, "return p", invalid);
        assert(!h.system->prepare());
        h.closed();
        assert(na1::constructed == 0U);
        std::printf("CASE cold-reject kind=%u objects=0 leases=0\n", invalid);
    }
    for (unsigned mode = 1U; mode <= 2U; ++mode)
    {
        na1::mode = mode;
        Harness h(1U);
        assert(h.system->prepare());
        assert(dispatchRuntimeHook(*h.system, h.hook) == 1U);
        if (mode == 2U)
            h.occurrence();
        assert(na1::completed == 0U && na1::frames_destroyed == 1U);
        assert(!h.system->failures().empty());
        assert(h.system->failures().front().status == (mode == 1U ? -771 : -772));
        h.closed();
        std::printf("CASE fail phase=%u completed=0 frames=1 unreachable=0\n", mode);
    }
    na1::mode = 0U;
    for (const auto body : {"error('step failure')", "return 'wrong result'", "coroutine.yield()",
                            "lux.Event.Task.event()", "lux.Ability.Delay.nextStep()"})
    {
        Harness h(1U, body);
        assert(h.system->prepare());
        assert(dispatchRuntimeHook(*h.system, h.hook) == 1U);
        h.occurrence();
        assert(na1::completed == 0U && !h.system->failures().empty());
        h.closed();
        std::printf("CASE step-error body=%s completed=0 frames=1\n", body);
    }
    {
        Harness h(1U, "local ok=pcall(function() lux.Event.Task.event() end); assert(not ok); return p");
        assert(h.system->prepare());
        assert(dispatchRuntimeHook(*h.system, h.hook) == 1U);
        h.occurrence();
        assert(na1::completed == 1U && na1::results[0] == 31 && h.system->failures().empty());
        h.closed();
        std::puts("CASE ordinary-error-recovery completed=1 result=31");
    }
    {
        Harness h(1U,
                  "do local x <close> = setmetatable({}, {__close=function() lux.Ability.TaskProbe.hit(3) end}) end; "
                  "return p");
        assert(h.system->prepare());
        assert(dispatchRuntimeHook(*h.system, h.hook) == 1U);
        h.occurrence();
        assert(h.probe.calls == 1U && na1::completed == 1U);
        h.closed();
        std::puts("CASE tbc-close calls=1 frame=1 cleanup=1");
    }
    {
        Harness h(64U);
        assert(h.system->prepare());
        assert(dispatchRuntimeHook(*h.system, h.hook) == 1U);
        h.closed();
        assert(na1::completed == 0U && na1::frames_destroyed == 64U);
        std::puts("CASE pending-cancel frames=64 completed=0 waiters=0");
    }
}

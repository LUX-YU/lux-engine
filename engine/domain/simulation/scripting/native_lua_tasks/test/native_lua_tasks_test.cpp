#include <lux/engine/function/script/lua/ScriptAbilityLua.hpp>
#include <lux/engine/simulation/scripting/cpp_static/ScriptDelayCoroutine.hpp>

#include "../../../builtin/script/test/ScriptRuntimeTestRegion.hpp"
#include "../../../builtin/script/test/ScriptTestClock.hpp"
#include "DelayAbility.ability.lua.generated.hpp"
#include "LuaValueTestTypes.lua.value.generated.hpp"
#include "NativeTask.Na1PoseTask.script.generated.hpp"
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
using lux::simulation::script::generated::Na1PoseTask;
using lux::simulation::script::generated::Na1Task;
lua_State *observed_vm{};
void *push_context{};
void (*push_reentry)(void *) noexcept {};
bool pushPose(lua_State *vm, const void *value) noexcept
{
    if (push_reentry)
        push_reentry(push_context);
    constexpr auto original = lux::script::lua::makeLuaValueOperation<ValuePose>();
    return original.push(vm, value);
}
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

struct FailingAllocation final
{
    lua_Alloc original{};
    void *context{};
    std::size_t permitted{}, failures{};
    static void *allocate(void *opaque, void *pointer, std::size_t old_size, std::size_t new_size) noexcept
    {
        auto &self = *static_cast<FailingAllocation *>(opaque);
        if (new_size > old_size || (!pointer && new_size != 0U))
        {
            if (self.permitted == 0U)
            {
                ++self.failures;
                return nullptr;
            }
            --self.permitted;
        }
        return self.original(self.context, pointer, old_size, new_size);
    }
};
lux::asset::AssetId asset(std::uint8_t value)
{
    std::array<std::uint8_t, 16U> bytes{};
    bytes[0] = value;
    return lux::asset::AssetId{bytes};
}
lux::script::ScriptArtifact nativeArtifact(const CppStaticContract &contract)
{
    auto description = materializeCppStaticScript(contract);
    assert(description);
    auto result = lux::script::ScriptArtifact::create(std::move(*description), {});
    assert(result);
    return std::move(*result);
}
lux::script::ScriptArtifact luaArtifact(std::string_view body, bool pose, unsigned invalid, std::string_view void_body)
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
    if (pose)
    {
        description.exports[0].args = {
            lux::rdesc::makeScriptValueType<ValuePose>(lux::semantic::EValuePass::CONST_REF)};
        description.exports[1].args = description.exports[0].args;
    }
    description.exports.push_back({"nested", 105U, {}, {}});
    description.exports.push_back({"apply_void", 106U, {lux::rdesc::makeScriptValueType<std::int32_t>()}, {}});
    description.exports.push_back({"other", 107U, {}, {}});
    description.lifecycle = {103U, 104U};
    const auto &probe = lux::script::ScriptAbilityTraits<na1::TaskProbe>::Description;
    const auto &delay = lux::script::ScriptAbilityTraits<DelayAbility>::Description;
    description.api_requirements = {{lux::script::ScriptApiContractId{probe.id.name()}, probe.schema_hash},
                                    {lux::script::ScriptApiContractId{delay.id.name()}, delay.schema_hash}};
    if (invalid == 4U)
        description.api_requirements.pop_back();
    description.body = lux::rdesc::LuaSourceScript{"Na1Lua", {101U, 107U}};
    const auto simulation = na1::domain();
    auto event =
        describeScriptEventSource<std::int32_t>(simulation.findEvent(na1::System, na1::Event), "Task", "event");
    assert(event);
    if (invalid != 3U)
        description.event_requirements.push_back(std::move(*event));
    const std::string source =
        "return { begin_life=function(self) assert(self.value==nil); "
        "self.value=1; lux.TaskProbe.hit(1) end, "
        "end_life=function(self,reason) assert(self.value>=1 and not "
        "self.ended); self.ended=true; "
        "lux.TaskProbe.hit(2) end, "
        "run=function(self) local p=lux.Event.Task.event(); "
        "self.value=self.value+p end, "
        "other=function(self) local p=lux.Event.Task.event(); assert(p==31); "
        "lux.TaskProbe.hit(4) end, "
        "apply_void=function(self,p) " +
        std::string(void_body.empty() ? "self.value=self.value+p; lux.TaskProbe.hit(3)" : void_body) +
        " end, "
        "nested=function(self) " +
        std::string(invalid == 8U ? "error('nested fault')" : "lux.TaskProbe.hit(4)") +
        " end, apply=function(self,p) " + std::string(body) + " end }";
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
                     unsigned invalid = 0U, bool pose = false, bool mixed = false, std::string_view void_body = {})
        : count(count), mixed(mixed), simulation(na1::domain()),
          lua_artifact(luaArtifact(body, pose, invalid, void_body)),
          native_artifact(nativeArtifact(pose ? Na1PoseTask : Na1Task))
    {
        na1::constructed = na1::destroyed = na1::started = na1::completed = na1::frames_destroyed = na1::unreachable =
            0U;
        na1::results.assign(count, 0);
        task_contract = pose ? Na1PoseTask : Na1Task;
        if (invalid == 13U)
            task_contract.sync_step_shapes = {};
        if (invalid == 14U)
        {
            static constexpr std::array wrong_shapes{scriptSyncStepShape<double(double)>()};
            task_contract.sync_step_shapes = wrong_shapes;
        }
        const auto& contract = task_contract;
        assert(pose_hook.prepare(count) == EEndpointMutationError::NONE);
        assert(nested_hook.prepare(count) == EEndpointMutationError::NONE);
        assert(hook.prepare(count) == EEndpointMutationError::NONE);
        assert(event.prepare({1U, 4U}) == EEndpointMutationError::NONE);
        hook_endpoint.emplace(na1::System, na1::Hook, hook);
        pose_endpoint.emplace(na1::System, na1::PoseHook, pose_hook);
        nested_endpoint.emplace(na1::System, na1::NestedHook, nested_hook);
        event_endpoint.emplace(na1::System, na1::Event, event);
        auto source = projectScriptEventSource(simulation.findEvent(na1::System, na1::Event),
                                               event_endpoint->descriptor(), "Task", "event");
        assert(source);
        event_source = std::move(*source);
        auto typed = CppScriptEventSource<std::int32_t>::create(contract, event_source);
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
        const std::array pools{CppStaticScriptPoolDescription{&contract, invalid == 12U ? 1U : count, count * 2U,
                                                              count * 2048U, alignof(std::max_align_t), count * 2U,
                                                              512U, true}};
        const auto native_run = pose                                   ? 101U
                                : na1::mode == 4U                      ? 108U
                                : na1::mode == 5U                      ? 109U
                                : (na1::mode == 6U || na1::mode == 7U) ? 110U
                                : na1::mode == 10U                     ? 112U
                                : na1::mode == 8U                      ? 111U
                                                                       : 101U;
        std::vector<NativeLuaTaskRoute> routes{{101U, native_run}};
        if (!pose && !mixed)
            routes.push_back({107U, 107U});
        std::array steps{*lua_artifact.findExport(102U), *lua_artifact.findExport(106U)};
        if (invalid == 2U)
            steps[0].returns[0] = lux::rdesc::makeScriptValueType<double>();
        if (invalid == 5U)
            steps[0] = *lua_artifact.findExport(101U);
        const std::array plans{NativeLuaTaskPlan{
            asset(1U), invalid == 1U ? native_artifact.contentIdentity() : lua_artifact.contentIdentity(), asset(2U),
            invalid == 11U ? lua_artifact.contentIdentity() : native_artifact.contentIdentity(), &contract, routes,
            steps}};
        std::array values{lux::script::lua::makeLuaValueOperation<ValuePose>()};
        if (pose)
            values[0].push = &pushPose;
        auto created = NativeLuaTaskBackend::create({.lua = {.instance_capacity = count,
                                                             .prepared_call_capacity = invalid == 6U ? 1U : count * 6U,
                                                             .continuation_capacity = mixed ? count : 0U,
                                                             .execution_depth_capacity = 8U,
                                                             .ability_catalog_method_capacity = 5U,
                                                             .prepared_ability_capacity = count * 5U,
                                                             .values = values,
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
                                                     .prepared_method_capacity = count * 5U});
        assert(created);
        backend.emplace(std::move(*created));
        descriptor = backend->descriptor();
        for (std::size_t i{}; i < count; ++i)
        {
            const auto entity = registry.create();
            entities.push_back(entity);
            mounts.push_back(
                {ScriptMountId{i + 1U},
                 asset(1U),
                 EntityScriptScope{entity},
                 {{101U, HookScriptTarget{na1::System, pose ? na1::PoseHook : na1::Hook}},
                  {na1::mode == 9U || mixed ? 107U : 105U, HookScriptTarget{na1::System, na1::NestedHook}}}});
        }
        const auto capacity = planScriptRuntimeCapacity(mounts);
        assert(capacity);
        const std::array endpoints{hook_endpoint->descriptor(), pose_endpoint->descriptor(),
                                   nested_endpoint->descriptor()};
        const auto events = event_endpoint->descriptor();
        const auto binding = lux::script::bindScriptAbility<na1::TaskProbe>(probe);
        const std::array capabilities{publishScriptAbility(binding)};
        auto runtime = ScriptSystem::create(
            simulation, *capacity, mounts, registry, clock_owner.clock(),
            {64U, count, invalid == 9U ? 1U : count * 2U, invalid == 9U ? 1U : 2U, invalid == 10U ? 1U : count * 2U,
             count * 2U, 64U, count * 2U, count * 2U, count * 2U, count * 2U, count * 2U},
            {this, &resolve}, capabilities, std::span{&descriptor, 1U}, endpoints, std::span{&events, 1U});
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
    struct TestPublication
    {
        ScriptInstanceId identity;
        std::uint64_t epoch{1U};
        ScriptSyncStepSetView view;
        PreparedScriptSyncStep step;
        std::size_t calls{};
        bool invalidate_in_call{};
    };
    void useCheckedPublication()
    {
        // A real CppStatic/runtime call with a test-owned producer of the public
        // synchronous-step view. This isolates the checked bridge from the facade's
        // immutable publication discipline.
        system.reset();
        publications.resize(count);
        const std::array pools{CppStaticScriptPoolDescription{&task_contract, count, count, count * 2048U,
                                                              alignof(std::max_align_t), count * 2U, 512U}};
        auto made = CppStaticScriptBackend::create(pools);
        assert(made);
        direct_native.emplace(std::move(*made));
        direct_api = direct_native->descriptor();
        descriptor = direct_api;
        descriptor.context = this;
        descriptor.createInstance = [](void *pointer, const ScriptInstanceCreateContext &context,
                                       const lux::script::ScriptArtifact &artifact,
                                       ScriptBackendInstance &output) noexcept {
            auto &h = *static_cast<Harness *>(pointer);
            auto &p = h.publications[h.publication_next++];
            p.identity = context.instance;
            p.step = {
                h.lua_artifact.findExport(102U), scriptSyncStepShape<std::int32_t(std::int32_t)>(), &p,
                +[](void* pointer, const void* const* arguments, void* output, const ScriptInvocationValidity&) noexcept
                {
                    auto& p = *static_cast<TestPublication*>(pointer);
                    ++p.calls;
                    const auto value = *static_cast<const std::int32_t*>(arguments[0]);
                    std::memcpy(output, &value, sizeof(value));
                    if (p.invalidate_in_call)
                        ++p.epoch;
                    return 0;
                }};
            p.view = {context.instance,
                      p.epoch,
                      context.behavior,
                      std::span{&p.step, 1U},
                      &p,
                      +[](const void *pointer, ScriptInstanceId identity, std::uint64_t epoch) noexcept {
                          const auto &p = *static_cast<const TestPublication *>(pointer);
                          return p.identity == identity && p.epoch == epoch;
                      }};
            auto child_context = context;
            child_context.sync_steps = &p.view;
            return h.direct_api.createInstance(h.direct_api.context, child_context, artifact, output);
        };
        descriptor.prepareMethod = [](void *pointer, ScriptBackendInstance instance,
                                      const lux::rdesc::ScriptFunction &function,
                                      ScriptBackendPreparedMethod &output) noexcept {
            const auto &api = static_cast<Harness *>(pointer)->direct_api;
            return api.prepareMethod(api.context, instance, function, output);
        };
        descriptor.releaseMethod = [](void *pointer, ScriptBackendInstance instance,
                                      ScriptBackendPreparedMethod method) noexcept {
            const auto &api = static_cast<Harness *>(pointer)->direct_api;
            api.releaseMethod(api.context, instance, method);
        };
        descriptor.destroyInstance = [](void *pointer, ScriptBackendInstance instance) noexcept {
            const auto &api = static_cast<Harness *>(pointer)->direct_api;
            api.destroyInstance(api.context, instance);
        };
        for (auto &mount : mounts)
        {
            mount.asset = asset(2U);
            mount.bindings.resize(1U);
        }
        const auto capacity = planScriptRuntimeCapacity(mounts);
        assert(capacity);
        const auto endpoint = hook_endpoint->descriptor();
        const auto event = event_endpoint->descriptor();
        auto made_system = ScriptSystem::create(
            simulation, *capacity, mounts, registry, clock_owner.clock(),
            {64U, count, count, 1U, count, count, 64U, count, count, count, count, count}, {this, &resolve}, {},
            std::span{&descriptor, 1U}, std::span{&endpoint, 1U}, std::span{&event, 1U});
        assert(made_system);
        system.emplace(std::move(*made_system));
    }
    void occurrence()
    {
        // One real owner step per occurrence; duplicate nonzero stable points
        // cannot drain resumes.
        clock_owner.advance(SimulationDuration{1'000'000});
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
        // Real backend counters, including attempts that did not reach a suspended
        // state.
        const auto expected_lua = mixed ? count : 0U;
        assert(child.lua.vm_coroutine_creations == expected_lua && child.lua.vm_coroutine_resumes == expected_lua);
        assert(child.lua.vm_coroutine_releases == expected_lua);
    }
    std::size_t count, leases{};
    bool mixed{};
    CppStaticContract task_contract;
    SimulationDescription simulation;
    lux::script::ScriptArtifact lua_artifact, native_artifact;
    lux::script::ScriptEventSourceDescription event_source;
    ecs::Registry registry;
    ScriptTestClock clock_owner{registry};
    Probe probe;
    std::vector<lux::script::lua::ScriptAbilityLuaMethodProjection> probe_methods;
    HookPoint<void()> hook, nested_hook;
    HookPoint<void(const ValuePose &)> pose_hook;
    HookChannel<SimulationBroadcastRoute, std::int32_t> event;
    std::optional<ScriptHookEndpoint<void()>> hook_endpoint, nested_endpoint;
    std::optional<ScriptHookEndpoint<void(const ValuePose &)>> pose_endpoint;
    std::optional<ScriptEventEndpoint<SimulationBroadcastRoute, std::int32_t>> event_endpoint;
    std::vector<ecs::Entity> entities;
    std::vector<ScriptRuntimeMount> mounts;
    std::optional<NativeLuaTaskBackend> backend;
    std::optional<CppStaticScriptBackend> direct_native;
    ScriptBackendDescriptor direct_api;
    std::vector<TestPublication> publications;
    std::size_t publication_next{};
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
    std::printf("CASE event count=%zu rounds=%u payload=31 completed=%zu "
                "lua_threads=0 lua_resumes=0 leases=%zu\n",
                count, rounds, na1::completed, h.leases);
}
} // namespace
int main()
{
    for (const auto invalid : {13U, 14U})
    {
        na1::mode = 0U;
        Harness h(1U, "return p", invalid);
        assert(!h.system->prepare());
        assert(na1::constructed == 0U && na1::destroyed == 0U && h.probe.begins == 0U && h.leases == 0U);
        assert(h.system->shutdown());
        std::printf("CASE cold-native-step-shape kind=%u objects=0 provider=0 leases=0\n", invalid);
    }
    for (const auto body : {"error('void failure')", "coroutine.yield()", "lux.Event.Task.event()",
                            "local x <close> = setmetatable({}, {__close=function() "
                            "lux.TaskProbe.hit(3) end}); error('close')"})
    {
        na1::mode = 8U;
        Harness h(1U, "return p", 0U, false, false, body);
        assert(h.system->prepare());
        assert(dispatchRuntimeHook(*h.system, h.hook) == 1U);
        h.occurrence();
        const auto expected_calls = std::string_view(body).starts_with("local x") ? 1U : 0U;
        assert(na1::completed == 0U && h.probe.calls == expected_calls && na1::frames_destroyed == 1U);
        assert(h.system->stats().active_awaitables == 0U && !h.system->failures().empty());
        h.closed();
        std::printf("CASE flat-void-failure body=%s provider=%zu frames=1 waits=0\n", body, expected_calls);
    }
    {
        na1::mode = 8U;
        Harness h(1U, "return p", 0U, false, false,
                  "local ok=pcall(function() lux.Delay.nextStep() end); assert(not ok); "
                  "assert(math.type(p)=='float'); lux.TaskProbe.hit(3)");
        assert(h.system->prepare());
        assert(dispatchRuntimeHook(*h.system, h.hook) == 1U);
        h.occurrence();
        assert(na1::completed == 1U && h.probe.calls == 1U && h.system->failures().empty());
        h.closed();
        std::puts("CASE flat-void-recovery provider=1 completed=1 "
                  "number-representation=float");
    }
    na1::mode = 0U;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    {
        auto empty = NativeLuaTaskBackend::create({});
        assert(!empty && empty.error() == ENativeLuaTaskBackendError::INVALID_CONFIGURATION);
        const std::array routes{NativeLuaTaskRoute{101U, 101U}};
        const std::array pools{CppStaticScriptPoolDescription{&Na1Task}};
        const std::array plans{NativeLuaTaskPlan{asset(1U), {}, asset(2U), {}, &Na1Task, routes, {}},
                               NativeLuaTaskPlan{asset(1U), {}, asset(2U), {}, &Na1Task, routes, {}}};
        auto duplicate = NativeLuaTaskBackend::create(
            {.native_pools = pools,
             .plans = plans,
             .artifacts = {nullptr, [](void *, const lux::asset::AssetId &,
                                       ResolvedScriptArtifact &) noexcept { return false; }},
             .instance_capacity = 1U,
             .prepared_method_capacity = 1U});
        assert(!duplicate && duplicate.error() == ENativeLuaTaskBackendError::DUPLICATE_PLAN);
        std::puts("CASE cold-factory invalid-capacity=1 duplicate-plan=1");
    }
    {
        Harness h(1U);
        ScriptBackendInstance invalid;
        const ScriptInstanceCreateContext context{asset(1U), EntityScriptScope{h.entities[0]}, nullptr, {1U, 1U}};
        const auto result = h.descriptor.createInstance(h.descriptor.context, context, h.lua_artifact, invalid);
        assert(result == EScriptBackendResult::HOST_CONTEXT_MISMATCH && !invalid && h.leases == 0U);
        assert(h.system->prepare());
        h.closed();
        std::puts("CASE host-reject leases=0 slot-reused=1");
    }
    normal(64U, 0U);
    normal(1000U, 0U);
    normal(64U, 3U);
    {
        Harness h(16U,
                  "self.value=self.value+p.key; return "
                  "p.key+p.mode+math.floor(p.velocity.x+p.velocity.y)",
                  0U, true);
        assert(h.system->prepare());
        ValuePose value{31, {2.5F, 7.0}, ValueMode::RUN};
        assert(dispatchRuntimeHook(*h.system, h.pose_hook, value) == 1U);
        for (auto result : na1::results)
            assert(result == 43);
        value = {-99, {-1.0F, -2.0}, ValueMode::WALK};
        h.occurrence();
        for (auto result : na1::results)
            assert(result == 74);
        assert(na1::completed == 16U && h.system->failures().empty());
        h.closed();
        std::puts("CASE record snapshot=43 after=74 caller-mutated=1 completed=16 "
                  "frames=16");
    }
    {
        na1::mode = 0U;
        Harness h(1U, "lux.TaskProbe.hit(3); return p");
        h.probe.callback_context = &h;
        h.probe.callback = [](void *pointer, std::int32_t code) noexcept {
            auto &h = *static_cast<Harness *>(pointer);
            if (code == 3)
                assert(dispatchRuntimeHook(*h.system, h.nested_hook) == 1U);
        };
        assert(h.system->prepare() && observed_vm);
        const auto base = lua_gettop(observed_vm);
        assert(dispatchRuntimeHook(*h.system, h.hook) == 1U);
        h.occurrence();
        std::fprintf(stderr, "TRACE nested done=%zu calls=%zu base=%d top=%d depth=%zu\n", na1::completed,
                     h.probe.calls, base, lua_gettop(observed_vm), h.backend->stats().lua.execution_depth_high_water);
        for (const auto &failure : h.system->failures())
            std::fprintf(stderr, "TRACE nested-failure error=%u status=%d\n", unsigned(failure.error), failure.status);
        assert(na1::completed == 1U && h.probe.calls == 2U && lua_gettop(observed_vm) == base);
        assert(h.system->failures().empty());
        h.closed();
        std::puts("CASE nested-lua provider=2 stack-delta=0 completed=1");
    }
    for (const bool fault : {false, true})
    {
        Harness h(1U, "lux.TaskProbe.hit(3); return p.key", fault ? 8U : 0U, true);
        assert(h.system->prepare());
        push_context = &h;
        push_reentry = fault ? +[](void* pointer) noexcept {
            push_reentry = nullptr;
            auto& h = *static_cast<Harness*>(pointer);
            assert(dispatchRuntimeHook(*h.system, h.nested_hook) == 1U);
        } : +[](void* pointer) noexcept {
            push_reentry = nullptr;
            auto& h = *static_cast<Harness*>(pointer);
            assert(h.system->requestStop());
        };
        const ValuePose value{31, {2.5F, 7.0}, ValueMode::RUN};
        assert(dispatchRuntimeHook(*h.system, h.pose_hook, value) == 1U);
        assert(push_reentry == nullptr && na1::started == 1U && na1::completed == 0U);
        if (fault)
        {
            assert(h.probe.calls == 0U && !h.system->failures().empty());
            assert(h.system->stats().active_awaitables == 0U && na1::frames_destroyed == 1U);
        }
        else
        {
            assert(h.probe.calls == 1U && h.system->failures().empty());
            assert(h.system->stats().active_awaitables == 1U && na1::frames_destroyed == 0U);
        }
        assert(h.system->processLifecycle());
        h.closed();
        std::printf("CASE converter-qualification fault=%u provider=%zu "
                    "completed=0 frames=1\n",
                    unsigned(fault), h.probe.calls);
    }
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
            const auto stats = h.system->stats();
            std::fprintf(stderr,
                         "TRACE sequence step=%llu C=%zu A=%zu event=%zu next=%zu "
                         "delay=%zu ready=%zu done=%zu\n",
                         h.clock_owner.clock().snapshot().step_index, stats.active_continuations,
                         stats.active_awaitables, stats.active_event_waiters, stats.next_step_waits,
                         stats.simulation_delay_waits, stats.resume_queue_depth, na1::completed);
            for (const auto &failure : h.system->failures())
                std::fprintf(stderr, "TRACE failure error=%u status=%d\n", unsigned(failure.error), failure.status);
            assert(stats.simulation_delay_waits == 64U && na1::completed == 0U);
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
    for (unsigned invalid = 1U; invalid <= 6U; ++invalid)
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
    for (const unsigned mode : {6U, 7U, 8U, 9U})
    {
        na1::mode = mode;
        Harness h(1U);
        assert(h.system->prepare());
        assert(dispatchRuntimeHook(*h.system, h.hook) == 1U);
        if (mode == 9U)
        {
            assert(dispatchRuntimeHook(*h.system, h.nested_hook) == 1U);
            assert(h.system->stats().active_continuations == 2U);
            h.occurrence();
            assert(na1::completed == 2U && na1::results[0] == 163);
        }
        else if (mode == 8U)
        {
            h.occurrence();
            assert(na1::completed == 1U && h.probe.calls == 1U);
        }
        else
        {
            assert(na1::completed == 0U && h.probe.calls == 0U);
            assert(h.system->stats().active_awaitables == 0U);
            assert(h.system->failures().size() == 1U);
            assert(h.system->failures()[0].status == (mode == 6U ? -32002 : -32003));
        }
        h.closed();
        std::printf("CASE typed-boundary mode=%u completed=%zu frames=%zu\n", mode, na1::completed,
                    na1::frames_destroyed);
    }
    na1::mode = 0U;
    for (const auto body : {"error('step failure')", "return 'wrong result'", "coroutine.yield()",
                            "lux.Event.Task.event()", "lux.Delay.nextStep()"})
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
        Harness h(1U, "local ok=pcall(function() lux.Event.Task.event() end); "
                      "assert(not ok); return p");
        assert(h.system->prepare());
        assert(dispatchRuntimeHook(*h.system, h.hook) == 1U);
        h.occurrence();
        assert(na1::completed == 1U && na1::results[0] == 31 && h.system->failures().empty());
        h.closed();
        std::puts("CASE ordinary-error-recovery completed=1 result=31");
    }
    {
        Harness h(1U, "do local x <close> = setmetatable({}, {__close=function() "
                      "lux.TaskProbe.hit(3) end}) end; "
                      "return p");
        assert(h.system->prepare());
        assert(dispatchRuntimeHook(*h.system, h.hook) == 1U);
        h.occurrence();
        assert(h.probe.calls == 1U && na1::completed == 1U);
        h.closed();
        std::puts("CASE tbc-close calls=1 frame=1 cleanup=1");
    }
    {
        Harness h(1U);
        assert(h.system->prepare());
        na1::results.resize(33U);
        const auto backing = h.backend->stats().composition_backing_bytes;
        for (unsigned round{}; round < 32U; ++round)
        {
            const auto old = (**h.system->queryMountStatus({1U})).instance;
            assert(dispatchRuntimeHook(*h.system, h.hook) == 1U);
            h.registry.destroy(h.entities[0]);
            assert(h.system->processLifecycle());
            assert(h.leases == 0U && h.backend->stats().native.active_frames == 0U);
            h.occurrence();
            assert(na1::completed == 0U);
            std::array<ScriptMountStatus, 1U> changes;
            assert(h.system->collectMountStatusChanges(changes));
            h.entities[0] = h.registry.create();
            h.mounts[0].scope = EntityScriptScope{h.entities[0]};
            assert(h.system->mountResolvedBatch(h.mounts));
            assert(h.system->processLifecycle());
            const auto current = (**h.system->queryMountStatus({1U})).instance;
            assert(current != old && h.backend->stats().composition_backing_bytes == backing);
        }
        assert(na1::frames_destroyed == 32U && h.probe.begins == 33U);
        h.closed();
        std::printf("CASE churn rounds=32 frames=32 begins=33 ends=33 "
                    "late-event-resumes=0 backing=%zu\n",
                    backing);
    }
    {
        Harness h(1U, "lux.TaskProbe.hit(3); lux.TaskProbe.hit(5); return p");
        h.probe.callback_context = &h;
        h.probe.callback = [](void *pointer, std::int32_t code) noexcept {
            auto &h = *static_cast<Harness *>(pointer);
            if (code != 3)
                return;
            assert(h.system->requestStop());
            assert(h.backend->stats().native.active_frames == 1U && h.leases == 2U);
            assert(na1::destroyed == 0U && h.probe.ends == 0U);
        };
        assert(h.system->prepare());
        assert(dispatchRuntimeHook(*h.system, h.hook) == 1U);
        h.occurrence();
        assert(na1::completed == 1U && h.probe.calls == 2U && !h.system->isShutdown());
        assert(h.system->processLifecycle());
        assert(h.system->isShutdown());
        h.closed();
        std::puts("CASE deferred-stop in-region-provider=2 completed=1 "
                  "early-destroy=0 final-destroy=1");
    }
    for (const std::size_t permitted : {0U, 1U, 3U})
    {
        Harness h(1U, "local t={}; for i=1,100 do t[i]=string.rep('x',10000) end; "
                      "lux.TaskProbe.hit(3); return #t");
        assert(h.system->prepare() && observed_vm);
        auto *vm = observed_vm;
        const auto base = lua_gettop(vm);
        assert(dispatchRuntimeHook(*h.system, h.hook) == 1U);
        FailingAllocation allocation;
        allocation.original = lua_getallocf(vm, &allocation.context);
        allocation.permitted = permitted;
        lua_setallocf(vm, &FailingAllocation::allocate, &allocation);
        h.occurrence();
        lua_setallocf(vm, allocation.original, allocation.context);
        assert(allocation.failures > 0U && lua_gettop(vm) == base);
        assert(h.probe.calls == 0U && na1::completed == 0U && na1::frames_destroyed == 1U);
        assert(!h.system->failures().empty());
        h.closed();
        std::printf("CASE step-oom permitted=%zu allocation-failures=%zu "
                    "provider=0 frames=1 stack-delta=0\n",
                    permitted, allocation.failures);
    }
    {
        na1::mode = 3U;
        Harness h(1U, "self.value=self.value+p; return self.value", 10U);
        assert(h.system->prepare());
        assert(dispatchRuntimeHook(*h.system, h.hook) == 1U);
        for (unsigned i{}; i < 32U; ++i)
            h.occurrence();
        assert(na1::started == 1U && na1::completed == 1U && na1::results[0] == 993);
        assert(h.system->failures().empty() && h.system->stats().active_awaitables == 0U);
        h.closed();
        std::puts("CASE one-wait-slot waits=32 tasks=1 result=993 frame-destroy=1");
    }
    {
        na1::mode = 4U;
        Harness h(2U, "self.value=self.value+p; return self.value", 9U);
        assert(h.system->prepare());
        assert(dispatchRuntimeHook(*h.system, h.hook) == 1U);
        assert(na1::started == 2U && na1::results[0] == 2 && na1::results[1] == 2);
        assert(h.system->stats().active_continuations == 1U && na1::frames_destroyed == 1U);
        assert(h.system->failures().size() == 1U);
        assert(h.system->failures()[0].error == EScriptSystemError::CONTINUATION_CAPACITY_EXCEEDED);
        h.closed();
        std::puts("CASE continuation-late capacity=1 starts=2 "
                  "before-wait-side-effects=2 frames=2");
    }
    {
        na1::mode = 0U;
        Harness h(1U, "return p", 0U, false, true);
        assert(h.system->prepare());
        assert(dispatchRuntimeHook(*h.system, h.hook) == 1U);
        assert(dispatchRuntimeHook(*h.system, h.nested_hook) == 1U);
        assert(h.system->stats().active_continuations == 2U);
        h.occurrence();
        assert(na1::completed == 1U && h.probe.calls == 1U);
        assert(h.system->failures().empty() && h.system->stats().backend_resume_calls == 2U);
        h.closed();
        std::puts("CASE mixed-routes native=1 Lua=1 one-identity=1 lifecycle=1 "
                  "frames=1 threads=1");
    }
    for (const unsigned invalid : {11U, 12U})
    {
        na1::mode = 0U;
        Harness h(invalid == 12U ? 2U : 1U, "return p", invalid);
        assert(!h.system->prepare());
        h.closed();
        assert(h.backend->stats().lua.prepared_ability_slots == 0U &&
               h.backend->stats().lua.prepared_event_slots == 0U);
        std::printf("CASE companion-reject kind=%u objects=%zu destroys=%zu leases=0\n", invalid, na1::constructed,
                    na1::destroyed);
    }
    {
        na1::mode = 10U;
        Harness h(1U);
        assert(h.system->prepare());
        assert(dispatchRuntimeHook(*h.system, h.hook) == 1U);
        assert(na1::started == 0U && na1::completed == 0U && na1::frames_destroyed == 0U);
        // The explicit frame-size guard runs before the storage allocator; its
        // counter stays zero.
        assert(h.backend->stats().native.frame_capacity_failures == 0U);
        assert(!h.system->failures().empty() && h.system->stats().active_awaitables == 0U);
        h.closed();
        std::puts("CASE large-record-frame records=16 limit=512 "
                  "size-limit-reject=1 storage-capacity-failures=0 body=0 "
                  "wait=0");
    }
    for (unsigned mode{}; mode < 4U; ++mode)
    {
        na1::mode = 0U;
        Harness h(1U);
        h.useCheckedPublication();
        assert(h.system->prepare());
        assert(dispatchRuntimeHook(*h.system, h.hook) == 1U);
        auto &p = h.publications[0];
        if (mode == 1U)
            ++p.epoch;
        else if (mode == 2U)
            p.view.instance = ScriptInstanceId{999U, 17U};
        else if (mode == 3U)
            p.invalidate_in_call = true;
        h.occurrence();
        assert(p.calls == ((mode == 0U || mode == 3U) ? 1U : 0U));
        assert(na1::completed == (mode == 0U ? 1U : 0U));
        assert(na1::results[0] == (mode == 0U ? 31 : 0));
        if (mode != 0U)
        {
            assert(h.system->failures().size() == 1U);
            assert(h.system->failures()[0].status == (mode == 3U ? -32004 : -32001));
        }
        h.closed();
        assert(h.direct_native->stats().active_frames == 0U);
        std::printf("CASE checked-publication mode=%u calls=%zu result=%d frames=1 "
                    "leases=0\n",
                    mode, p.calls, na1::results[0]);
    }
    for (const auto expression : {"true", "0/0", "math.huge", "1.5", "2147483648"})
    {
        na1::mode = 0U;
        const std::string body = "lux.TaskProbe.hit(3); return " + std::string(expression);
        Harness h(1U, body);
        assert(h.system->prepare());
        assert(dispatchRuntimeHook(*h.system, h.hook) == 1U);
        h.occurrence();
        assert(na1::completed == 0U && na1::results[0] == 0 && h.probe.calls == 1U);
        assert(!h.system->failures().empty());
        h.closed();
        std::printf("CASE output-reject value=%s provider=1 published-result=0 frames=1\n", expression);
    }
    {
        na1::mode = 0U;
        Harness h(1U, "local x <close> = setmetatable({}, {__close=function() "
                      "lux.TaskProbe.hit(3); "
                      "error('close failure') end}); return p");
        assert(h.system->prepare() && observed_vm);
        const auto base = lua_gettop(observed_vm);
        assert(dispatchRuntimeHook(*h.system, h.hook) == 1U);
        h.occurrence();
        assert(na1::completed == 0U && h.probe.calls == 1U && na1::results[0] == 0);
        assert(lua_gettop(observed_vm) == base && !h.system->failures().empty());
        h.closed();
        std::puts("CASE close-error provider=1 result-unpublished=1 stack-restored=1");
    }
    {
        na1::mode = 0U;
        Harness h(1U, "lux.TaskProbe.hit(3); return p.key", 0U, true);
        assert(h.system->prepare() && observed_vm);
        auto *vm = observed_vm;
        const auto base = lua_gettop(vm);
        FailingAllocation allocation;
        allocation.original = lua_getallocf(vm, &allocation.context);
        lua_setallocf(vm, &FailingAllocation::allocate, &allocation);
        const ValuePose value{31, {2.5F, 7.0}, ValueMode::RUN};
        assert(dispatchRuntimeHook(*h.system, h.pose_hook, value) == 1U);
        lua_setallocf(vm, allocation.original, allocation.context);
        assert(allocation.failures && h.probe.calls == 0U && na1::completed == 0U);
        assert(na1::frames_destroyed == 1U && lua_gettop(vm) == base);
        assert(h.system->stats().active_awaitables == 0U && !h.system->failures().empty());
        h.closed();
        std::printf("CASE record-argument-oom failures=%zu provider=0 frames=1 "
                    "stack-restored=1\n",
                    allocation.failures);
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

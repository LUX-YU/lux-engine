#include "../../../builtin/script/test/ScriptRuntimeTestRegion.hpp"
using lux::simulation::script::test::dispatchRuntimeHook;
using lux::simulation::script::test::deliverRuntimeEvent;
using lux::simulation::script::test::executeRuntimeStablePoint;
#include "../../../system/test/HookInvocationTestAccess.hpp"
#include "LuaValueTestTypes.lua.value.generated.hpp"
#include "LuaValueTestAbility.ability.generated.hpp"
#include "LuaValueTestAbility.ability.lua.generated.hpp"
#include "LuaPushOnlyAbility.ability.generated.hpp"
#include "LuaPushOnlyAbility.ability.lua.generated.hpp"
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/scripting/DeferredScriptHost.hpp>
#include <lux/engine/simulation/scripting/ScriptLifecycle.hpp>
#include <lux/engine/simulation/scripting/ScriptEventSource.hpp>
#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

using namespace lux::simulation;
using namespace lux::simulation::script;
using namespace lux::simulation::script::test;
using lux::simulation::test::dispatchHookForTest;
using AbilityTraits = lux::script::ScriptAbilityTraits<LuaValueTestAbility>;
constexpr lux::system::SystemInstanceId kOwner{0x5A0101};
constexpr HookPointId kHook{0x5A0102};
constexpr HookPointId kFaultHook{0x5A0106};
constexpr std::uint64_t kScoreComponent{0x5A0107};
static const std::array kHostComponents{scriptDeferredComponent<std::int32_t>({
    kScoreComponent, lux::semantic::typeId("lux.i32"), "lux.i32", LUX_SCRIPT_VK_INT32
})};
constexpr lux::script::ScriptSymbolId kTick{0x5A0103}, kBegin{0x5A0104}, kEnd{0x5A0105};

struct Provider
{
    std::size_t poses{}, angles{}, tokens{};
    std::size_t scalar_calls{}, zero_calls{};
    bool invalid_result{};
    ValuePose echo(const ValuePose& input) noexcept
    {
        ++poses;
        return {input.id + 1, {input.velocity.x * 2, input.velocity.y + 3},
            invalid_result ? static_cast<ValueMode>(99) : input.mode};
    }
    ValueAngle angle(ValueAngle input) noexcept { ++angles; return input; }
    std::int32_t inspect(const ValueConstRecord& input) noexcept
    { return input.id + static_cast<std::int32_t>(input.weight); }
    std::int32_t token(const ValueToken& input, std::int32_t next) noexcept
    { ++tokens; return input.value() + next; }
    std::int32_t scalarProbe(std::int32_t value) noexcept { ++scalar_calls; return value + 1; }
    std::int32_t zeroArgumentProbe() noexcept { ++zero_calls; return 42; }
};
static SimulationDescription simulation(bool fault = false)
{
    const std::array hooks{makeHookPointSpec<void()>(kHook, "value-tick"),
        makeHookPointSpec<void()>(kFaultHook, "value-nested-fault")};
    const SimulationSystemDescription description{
        .type = {.canonical_name = "lux.test.values", .version = 1}, .hooks = std::span{hooks}.first(fault ? 2U : 1U)};
    SimulationDescriptionBuilder builder;
    assert(builder.addSystem(kOwner, "values", description));
    auto value = std::move(builder).build();
    assert(value);
    return std::move(*value);
}
static lux::script::ScriptArtifact artifact(std::string_view tick)
{
    lux::rdesc::Script description;
    description.module_name = "lux.test.values";
    description.body = lux::rdesc::LuaSourceScript{"Values"};
    description.api_requirements.push_back({
        lux::script::ScriptApiContractId{AbilityTraits::Description.id.name()},
        AbilityTraits::Description.schema_hash
    });
    description.exports = {{"tick", kTick, {}, {}}, {"begin_life", kBegin, {}, {}},
        {"end_life", kEnd, {lux::rdesc::makeScriptValueType<EScriptEndPlayReason>()}, {}}};
    description.lifecycle = {kBegin, kEnd};
    const std::string source =
        "return {begin_life=function(self) assert(math.abs(lux.Values.angle(90)-90)<0.001) end, "
        "end_life=function(self,reason) assert(math.abs(lux.Values.angle(180)-180)<0.001) end, "
        "tick=function(self) " +
        std::string{tick} + " end}";
    const auto bytes = std::as_bytes(std::span{source.data(), source.size()});
    auto value = lux::script::ScriptArtifact::create(std::move(description), {bytes.begin(), bytes.end()});
    assert(value);
    return std::move(*value);
}
struct Runtime
{
    LuaScriptBackend& backend;
    SimulationDescription sim = simulation();
    lux::script::ScriptArtifact script;
    lux::asset::AssetId asset;
    ecs::Registry registry;
    ecs::Entity entity = registry.create();
    SimulationClock clock;
    HookPoint<void()> hook;
    HookPoint<void()> fault_hook;
    std::optional<ScriptHookEndpoint<void()>> endpoint;
    std::optional<ScriptHookEndpoint<void()>> fault_endpoint;
    Provider provider;
    decltype(lux::script::bindScriptAbility<LuaValueTestAbility>(provider)) binding;
    std::optional<DeferredScriptHost> host;
    std::optional<ScriptSystem> system;
    Runtime(LuaScriptBackend& backend, std::uint8_t id, std::string_view tick,
        bool fault = false, bool deferred = false)
        : backend(backend), sim(simulation(fault)), script(artifact(tick)),
          binding(lux::script::bindScriptAbility<LuaValueTestAbility>(provider))
    {
        if (deferred)
        {
            registry.emplace<std::int32_t>(entity, 10);
            host.emplace(registry, kHostComponents);
        }
        std::array<std::uint8_t, 16> bytes{};
        static std::uint8_t incarnation{};
        bytes[0] = id;
        bytes[1] = ++incarnation; // Different source text is a different asset, never a hot reload.
        asset = lux::asset::AssetId{bytes};
        assert(hook.prepare(1) == EEndpointMutationError::NONE);
        endpoint.emplace(kOwner, kHook, hook);
        std::array mounts{ScriptRuntimeMount{ScriptMountId{1}, asset, EntityScriptScope{entity},
            {{kTick, HookScriptTarget{kOwner, kHook}}}}};
        if (fault)
        {
            assert(fault_hook.prepare(1) == EEndpointMutationError::NONE);
            fault_endpoint.emplace(kOwner, kFaultHook, fault_hook);
            mounts.front().bindings.push_back({kTick, HookScriptTarget{kOwner, kFaultHook}});
        }
        const auto plan = planScriptRuntimeCapacity(mounts);
        assert(plan);
        const std::array capabilities{publishScriptAbility(binding)};
        const auto descriptor = backend.descriptor();
        const std::array endpoints{endpoint->descriptor(), fault ? fault_endpoint->descriptor() :
            ScriptHookEndpointDescriptor{}};
        auto created = ScriptSystem::create(sim, *plan, mounts, registry, clock,
            {8U, 1U, 4U, 4U, 4U, 4U, 64U, 4U, 4U, 4U, 4U, 4U},
            {this, [](void* context, const lux::asset::AssetId& requested, ResolvedScriptArtifact& output) noexcept {
                auto& self = *static_cast<Runtime*>(context);
                if (requested != self.asset) return false;
                output.artifact = &self.script;
                return true;
            }}, capabilities, std::span{&descriptor, 1}, std::span{endpoints}.first(fault ? 2U : 1U), {},
            host ? host->api() : ScriptHostApi{});
        assert(created);
        system.emplace(std::move(*created));
        std::fprintf(stderr, "VALUE_CASE asset=%u\n", static_cast<unsigned>(bytes[1]));
        const auto prepared = system->prepare();
        if (!prepared)
        {
            std::fprintf(stderr, "VALUE_PREPARE error=%u angles=%zu failures=%zu\n",
                static_cast<unsigned>(prepared.error()), provider.angles, system->failures().size());
            for (const auto& failure : system->failures())
                std::fprintf(stderr, "VALUE_FAILURE status=%d\n", failure.status);
        }
        assert(prepared);
        assert(provider.angles == 1); // BeginPlay is not ACTIVE, but is an authorized lifecycle call.
    }
};

static void deferredHostCase()
{
    const auto contribution = lux::script::lua::makeScriptAbilityLuaContribution<LuaValueTestAbility>();
    const std::array components{LuaComponentBinding{"score", kScoreComponent, lux::semantic::typeId("lux.i32"),
        "lux.i32", LUX_SCRIPT_VK_INT32, sizeof(std::int32_t), alignof(std::int32_t)}};
    auto backend = LuaScriptBackend::create({
        .instance_capacity = 1, .prepared_call_capacity = 8, .continuation_capacity = 1,
        .execution_depth_capacity = 4, .ability_catalog_method_capacity = AbilityTraits::Methods.size(),
        .prepared_ability_capacity = AbilityTraits::Methods.size(), .components = components,
        .abilities = std::span{&contribution, 1},
        .prepared_ability_blocks = std::array{LuaPreparedBlockClass{AbilityTraits::Methods.size(), 1}},
        .prepared_ability_storage_bytes = 65536});
    assert(backend);
    for (const bool fail : {false, true})
    {
        const std::string source = "assert(self:get_component('score')==10);"
            "assert(self:patch_component('score',25)); assert(self:get_component('score')==10);"
            "assert(not self:patch_component('score','bad')); assert(self:destroy());"
            "assert(self:has_component('score')); assert(lux.Values.scalarProbe(7)==8);" +
            std::string{fail ? "error('after accepted commands')" : ""};
        Runtime runtime{*backend, 3, source, false, true};
        ecs::EcsCommandBuffer commands;
        const std::array capacities{ecs::EcsCommandProducerCapacity{2, 64}};
        assert(commands.prepare(capacities));
        {
            auto writer = commands.begin(0, ecs::EEcsCommandPolicy::CONTINUE_ON_INVALID_TARGET);
            assert(writer);
            auto host = runtime.host->begin(*writer);
            auto region = runtime.system->beginExecutionRegion();
            assert(host && region && dispatchRuntimeHook(*runtime.system, runtime.hook) == 1U);
            assert(runtime.provider.scalar_calls == 1U && runtime.registry.valid(runtime.entity));
            assert(runtime.registry.get<std::int32_t>(runtime.entity) == 10);
            assert(runtime.host->accepted() == 2U && runtime.host->rejected() == 0U);
            assert(runtime.system->failures().size() == static_cast<std::size_t>(fail));
            assert(region->finish());
        }
        assert(ecs::applyEcsCommands(runtime.registry, commands) && !runtime.registry.valid(runtime.entity));
        assert(runtime.system->processLifecycle() && runtime.system->activeInstanceCount() == 0U);
        assert(runtime.provider.angles == 2U && backend->stats().prepared_ability_slots == 0U);
        assert(runtime.system->shutdown());
        std::printf("LUA_DEFERRED fault=%u accepted=2 provider=1 endplay=1 backlog=0 PASS\n", fail);
    }
}
static Runtime* outer;
static Runtime* nested;
static void reenter() noexcept
{
    value_reentry = nullptr;
    const auto previous = nested->provider.poses;
    assert(dispatchRuntimeHook(*nested->system, nested->hook) == 1);
    assert(nested->provider.poses == previous + 1);
}
static void retire() noexcept
{
    value_reentry = nullptr;
    outer->registry.destroy(outer->entity); // Actual structural invalidation, not a deferred request.
    assert(!outer->registry.valid(outer->entity));
    const auto status = outer->system->queryMountStatus({1});
    assert(!status && status.error() == EScriptSystemError::ENDPOINT_BUSY);
    assert(outer->backend.stats().prepared_ability_slots >= AbilityTraits::Methods.size());
}
static void closeDuringRead() noexcept
{
    value_reentry = nullptr;
    assert(outer->system->requestStop());
    assert(!outer->system->shutdown()); // A request does not revoke this region's invocation authority.
    assert(outer->backend.stats().prepared_ability_slots >= AbilityTraits::Methods.size());
}
static void faultDuringRead() noexcept
{
    value_reentry = nullptr;
    // A real nested Lua invocation fails, faulting the same incarnation while the outer pcall is still running.
    assert(dispatchRuntimeHook(*outer->system, outer->fault_hook) == 1);
    assert(outer->system->failures().size() == 1U && outer->system->activeInstanceCount() == 0U);
}
static constexpr std::string_view pose_tick =
    "local p=lux.Values.echo({key=7,velocity={x=1.5,y=2.25},mode=3});"
    "assert(p.key==8 and p.velocity.x==3 and p.velocity.y==5.25 and p.mode==3)";
static void admissionCase(LuaScriptBackend& backend, std::string_view name)
{
    const bool stopping = name.starts_with("stop-");
    const bool retiring = name.starts_with("retire-");
    const bool faulting = name.starts_with("fault-");
    const bool zero = name.ends_with("zero");
    assert(stopping || retiring || faulting || name == "input-recovery");
    const bool recovery = !stopping && !retiring && !faulting;
    const bool callable = recovery || stopping;
    const std::string prefix = recovery ? "assert(not pcall(lux.Values.angle,'bad'));" :
        stopping ? "assert(pcall(lux.Values.angle,90));" : "assert(not pcall(lux.Values.angle,90));";
    const std::string call = zero ? "lux.Values.zeroArgumentProbe" : "lux.Values.scalarProbe,7";
    const std::string fault_prefix = faulting ?
        "if self.faulting then error('nested failure') end self.faulting=true;" : "";
    const std::string script = fault_prefix + prefix + "local ok,v=pcall(" + call + ");"
        "print('ADMISSION_LUA " + std::string{name} + " reached ok='..tostring(ok));" +
        (callable ? zero ? "assert(ok and v==42)" : "assert(ok and v==8)" : "assert(not ok)") +
        (recovery ? "assert(lux.Values.zeroArgumentProbe()==42)" : "");
    std::fprintf(stderr, "ADMISSION_BEGIN %.*s\n", static_cast<int>(name.size()), name.data());
    Runtime runtime{backend, 1, script, faulting};
    outer = &runtime;
    value_reentry = stopping ? closeDuringRead : retiring ? retire : faulting ? faultDuringRead : nullptr;
    assert(dispatchRuntimeHook(*runtime.system, runtime.hook) == 1);
    std::fflush(stdout);
    std::fprintf(stderr, "ADMISSION_COUNTS %.*s scalar=%zu zero=%zu angles=%zu failures=%zu\n",
        static_cast<int>(name.size()), name.data(), runtime.provider.scalar_calls, runtime.provider.zero_calls,
        runtime.provider.angles, runtime.system->failures().size());
    assert(runtime.provider.scalar_calls == static_cast<std::size_t>(callable && !zero));
    assert(runtime.provider.zero_calls == static_cast<std::size_t>(recovery || (stopping && zero)));
    assert(runtime.system->failures().size() == static_cast<std::size_t>(faulting));
    assert(runtime.system->stats().invocation_failures == static_cast<std::size_t>(faulting));
    assert(runtime.system->shutdown());
    assert(runtime.provider.angles == (stopping ? 3U : 2U));
    assert(runtime.system->activeContinuationCount() == 0);
    assert(backend.stats().prepared_ability_slots == 0);
    std::fprintf(stderr, "ADMISSION_PASS %.*s endplay=1 backlog=0\n", static_cast<int>(name.size()), name.data());
}
static void standaloneCase(LuaScriptBackend& backend)
{
    const auto script = artifact("assert(lux.Values.scalarProbe(7)==8);"
        "assert(lux.Values.zeroArgumentProbe()==42); assert(math.abs(lux.Values.angle(90)-90)<0.001)");
    Provider provider;
    const auto binding = lux::script::bindScriptAbility<LuaValueTestAbility>(provider);
    const auto published = publishScriptAbility(binding);
    const PreparedScriptApiCapability capability{lux::script::ScriptApiContractId{published.contract.name()},
        published.schema_hash, published.context, published.dispatch, published.schema_version, published.methods};
    std::array<std::uint8_t, 16> bytes{};
    bytes[0] = 99;
    const auto descriptor = backend.descriptor();
    ScriptBehavior behavior;
    assert(!behavior.hasInvocationAuthority());
    for (auto* host : {static_cast<ScriptBehavior*>(nullptr), &behavior})
    {
        ScriptBackendInstance instance;
        assert(descriptor.createInstance(descriptor.context,
            {lux::asset::AssetId{bytes}, SimulationScriptScope{}, host, {}, std::span{&capability, 1}},
            script, instance) == EScriptBackendResult::SUCCESS);
        ScriptBackendPreparedMethod tick;
        assert(descriptor.prepareMethod(descriptor.context, instance, script.description().exports[0], tick) ==
            EScriptBackendResult::SUCCESS);
        lux_script_call_frame frame{nullptr, 0, 0, nullptr, 0, 0, nullptr};
        assert(tick.synchronous.invoke(tick.synchronous.context, &frame) == 0);
        descriptor.releaseMethod(descriptor.context, instance, tick);
        descriptor.destroyInstance(descriptor.context, instance);
    }
    assert(provider.scalar_calls == 2 && provider.zero_calls == 2 && provider.angles == 2);
    std::puts("ADMISSION_STANDALONE null-host unbound-host scalar=2 zero=2 custom=2 PASS");
}
static ecs::Registry* resume_registry{};
static ecs::Entity resume_entity;
static ScriptSystem* resume_system{};
static bool resume_stop{};
static std::size_t resume_conversions{};

static int resumeAuthorityCase(bool stop)
{
    constexpr EventPointId event_id{0x5A0120};
    constexpr HookPointId dispatch_id{0x5A0121};
    const std::array hooks{makeHookPointSpec<void()>(kHook, "resume-start"),
        makeHookPointSpec<void()>(dispatch_id, "resume-delivery")};
    const std::array events{makeEventPointSpec<ValuePose>(event_id, "resume-pose", dispatch_id,
        EEventRoute::SIMULATION_BROADCAST, "lux.test.lua.pose", 1U)};
    SimulationDescriptionBuilder builder;
    assert(builder.addSystem(kOwner, "resume", {.type = {.canonical_name = "lux.test.resume", .version = 1},
        .hooks = hooks, .events = events}));
    auto sim = std::move(builder).build();
    assert(sim);
    HookPoint<void()> hook;
    assert(hook.prepare(1U) == EEndpointMutationError::NONE);
    ScriptHookEndpoint<void()> endpoint{kOwner, kHook, hook};
    HookChannel<SimulationBroadcastRoute, ValuePose> channel;
    assert(channel.prepare({1U, 1U}, [](const ValuePose& value) noexcept { return value; }) ==
        EEndpointMutationError::NONE);
    ScriptEventEndpoint<SimulationBroadcastRoute, ValuePose> event{kOwner, event_id, channel,
        [](const ValuePose& value, std::span<std::byte> output) noexcept {
            if (output.size() != sizeof(value)) return false;
            std::memcpy(output.data(), &value, sizeof(value));
            return true;
        }};
    auto source = projectScriptEventSource(sim->findEvent(kOwner, event_id), event.descriptor(), "Resume", "pose");
    assert(source);
    const auto contribution = lux::script::lua::makeScriptAbilityLuaContribution<LuaValueTestAbility>();
    auto operation = lux::script::lua::makeLuaValueOperation<ValuePose>();
    operation.push = [](lua_State* state, const void* value) noexcept {
        ++resume_conversions;
        if (resume_stop) assert(resume_system->requestStop());
        else resume_registry->destroy(resume_entity);
        const auto original = lux::script::lua::makeLuaValueOperation<ValuePose>();
        return original.push(state, value);
    };
    auto backend = LuaScriptBackend::create({
        .instance_capacity = 1U, .prepared_call_capacity = 3U, .continuation_capacity = 1U,
        .execution_depth_capacity = 4U, .ability_catalog_method_capacity = AbilityTraits::Methods.size(),
        .prepared_ability_capacity = AbilityTraits::Methods.size(), .values = std::span{&operation, 1},
        .abilities = std::span{&contribution, 1}, .event_catalog_capacity = 1U, .prepared_event_capacity = 1U,
        .events = std::span{&*source, 1},
        .prepared_ability_blocks = std::array{LuaPreparedBlockClass{AbilityTraits::Methods.size(), 1U}},
        .prepared_ability_storage_bytes = 1024U * 1024U,
        .prepared_event_blocks = std::array{LuaPreparedBlockClass{1U, 1U}},
        .prepared_event_storage_bytes = 1024U * 1024U
    });
    assert(backend);
    lux::rdesc::Script description;
    description.module_name = "lux.test.resume-authority";
    description.body = lux::rdesc::LuaSourceScript{"Resume", {kTick}};
    description.exports = {{"tick", kTick, {}, {}}};
    description.api_requirements = {{lux::script::ScriptApiContractId{AbilityTraits::Description.id.name()},
        AbilityTraits::Description.schema_hash}};
    description.event_requirements = {*source};
    constexpr std::string_view code =
        "return {tick=function(self) "
        "assert(debug.getinfo(lux.Event.Resume.pose,'S').what=='C');"
        "assert(debug.getinfo(lux.Values.zeroArgumentProbe,'S').what=='C');"
        "local saved=coroutine.yield; coroutine.yield=function() error('intercepted engine primitive') end;"
        "local p=lux.Event.Resume.pose(); coroutine.yield=saved;"
        "assert(p.key==31 and p.velocity.x==2 and p.velocity.y==3 and p.mode==3);"
        "assert(lux.Values.zeroArgumentProbe()==42) end}";
    const auto bytes = std::as_bytes(std::span{code.data(), code.size()});
    auto script = lux::script::ScriptArtifact::create(std::move(description), {bytes.begin(), bytes.end()});
    assert(script);
    std::array<std::uint8_t, 16> id{};
    id[0] = 41U;
    const lux::asset::AssetId asset{id};
    ecs::Registry registry;
    const auto entity = registry.create();
    SimulationClock clock;
    Provider provider;
    const auto binding = lux::script::bindScriptAbility<LuaValueTestAbility>(provider);
    const std::array capabilities{publishScriptAbility(binding)};
    const std::array mounts{ScriptRuntimeMount{ScriptMountId{1}, asset, EntityScriptScope{entity},
        {{kTick, HookScriptTarget{kOwner, kHook}}}}};
    const auto plan = planScriptRuntimeCapacity(mounts);
    assert(plan);
    const auto descriptor = backend->descriptor();
    const auto hook_descriptor = endpoint.descriptor();
    const auto event_descriptor = event.descriptor();
    auto system = ScriptSystem::create(*sim, *plan, mounts, registry, clock,
        {8U, 1U, 4U, 4U, 4U, 4U, 64U, 4U, 4U, 4U, 4U, 4U},
        {&*script, [](void* context, const lux::asset::AssetId&, ResolvedScriptArtifact& output) noexcept {
            output.artifact = static_cast<lux::script::ScriptArtifact*>(context);
            return true;
        }}, capabilities, std::span{&descriptor, 1}, std::span{&hook_descriptor, 1},
        std::span{&event_descriptor, 1});
    assert(system && system->prepare());
    assert(dispatchRuntimeHook(*system, hook) == 1U);
    assert(system->activeContinuationCount() == 1U && system->stats().active_event_waiters == 1U);
    {
        auto writer = channel.begin(0U);
        assert(writer.record(ValuePose{31, {2.0F, 3.0}, ValueMode::RUN}));
    }
    assert(deliverRuntimeEvent(*system, event) == 1U);
    resume_registry = &registry;
    resume_entity = entity;
    resume_system = &*system;
    resume_stop = stop;
    resume_conversions = 0U;
    const auto stable = executeRuntimeStablePoint(*system);
    assert(stable);
    assert(resume_conversions == 1U && provider.zero_calls == static_cast<std::size_t>(stop));
    assert(system->activeContinuationCount() == 0U && system->activeAwaitableCount() == 0U);
    assert(system->shutdown());
    const auto released = backend->stats();
    assert(released.vm_coroutine_creations == 1U && released.vm_coroutine_releases == 1U);
    assert(released.prepared_ability_slots == 0U && released.prepared_event_slots == 0U);
    std::printf("RESUME_AUTHORITY stop=%u converted=1 provider=%zu roots=1 released=1 backlog=0 PASS\n",
        stop, provider.zero_calls);
    const auto leaf_stats = backend->stats();
    std::printf("ENGINE_LEAF,available=%d,observed=%d,fast=%llu,standard=%llu\n",
        leaf_stats.leaf_yield_available, leaf_stats.leaf_statistics_enabled,
        leaf_stats.leaf_return_yields, leaf_stats.standard_leaf_yields
    );
    if (leaf_stats.leaf_statistics_enabled) assert(leaf_stats.leaf_return_yields == 1U);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view{argv[1]} == "--resume-retire") return resumeAuthorityCase(false);
    if (argc == 2 && std::string_view{argv[1]} == "--resume-stop") return resumeAuthorityCase(true);
    static_assert(!std::is_default_constructible_v<ValueToken>);
    static_assert(!lux::script::lua::LuaValueCodec<ValuePushOnly>::can_read);
    const auto push_only = lux::script::lua::makeScriptAbilityLuaContribution<LuaPushOnlyAbility>();
    const auto rejected = LuaScriptBackend::create({.instance_capacity = 1, .prepared_call_capacity = 3,
        .continuation_capacity = 1, .execution_depth_capacity = 2, .ability_catalog_method_capacity = 1,
        .abilities = std::span{&push_only, 1}});
    assert(!rejected && rejected.error() == ELuaScriptBindingBackendError::UNSUPPORTED_ABILITY_TYPE);
    const auto contribution = lux::script::lua::makeScriptAbilityLuaContribution<LuaValueTestAbility>();
    auto incompatible = lux::script::lua::makeLuaValueOperation<ValuePose>();
    ++incompatible.representation;
    const auto conflict = LuaScriptBackend::create({.instance_capacity = 1, .prepared_call_capacity = 3,
        .continuation_capacity = 1, .execution_depth_capacity = 2,
        .ability_catalog_method_capacity = AbilityTraits::Methods.size(),
        .values = std::span{&incompatible, 1}, .abilities = std::span{&contribution, 1}});
    assert(!conflict && conflict.error() == ELuaScriptBindingBackendError::INVALID_VALUE_OPERATION);

    const bool benchmark_mode = argc > 2 && std::string_view{argv[1]} == "--benchmark";
    const bool vm_accounting = !benchmark_mode || (argc > 3 && std::string_view{argv[3]} == "--allocations");
    auto created = LuaScriptBackend::create({
        // Conformance adds distinct immutable assets for fault/stop cases; timed work retains the original capacity.
        .instance_capacity = 2, .prepared_call_capacity = benchmark_mode ? 64U : 96U, .continuation_capacity = 2,
        .execution_depth_capacity = 8, .ability_catalog_method_capacity = AbilityTraits::Methods.size(),
        .prepared_ability_capacity = 2 * AbilityTraits::Methods.size(), .abilities = std::span{&contribution, 1},
        .track_vm_allocations = vm_accounting,
        .prepared_ability_blocks = std::array{LuaPreparedBlockClass{AbilityTraits::Methods.size(), 2}},
        .prepared_ability_storage_bytes = 1024 * 1024});
    assert(created);
    auto backend = std::move(*created);
    if (argc > 2 && std::string_view{argv[1]} == "--admission-case")
    {
        admissionCase(backend, argv[2]);
        return 0;
    }
    if (benchmark_mode)
    {
        const auto count = std::strtoull(argv[2], nullptr, 10);
        Runtime runtime{backend, 1, pose_tick};
        for (int i{}; i < 1000; ++i) assert(dispatchRuntimeHook(*runtime.system, runtime.hook) == 1);
        const auto before = backend.stats();
        const auto begin = std::chrono::steady_clock::now();
        for (std::size_t i{}; i < count; ++i) assert(dispatchRuntimeHook(*runtime.system, runtime.hook) == 1);
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - begin).count();
        assert(runtime.provider.poses == count + 1000 && runtime.system->failures().empty());
        const auto after = backend.stats();
        std::printf("VALUE_BENCH count=%llu ns=%lld operations=%zu errors=0 backlog=%zu "
            "vm_accounting=%d vm_allocations=%llu\n",
            count, elapsed, runtime.provider.poses - 1000, runtime.system->activeContinuationCount(),
            vm_accounting, after.vm_allocations.allocations - before.vm_allocations.allocations);
        assert(runtime.system->stats().invocation_failures == 0U);
        assert(runtime.system->shutdown() && runtime.provider.angles == 2U);
        const auto closed = runtime.system->stats();
        const auto released = backend.stats();
        assert(closed.invocation_failures == 0U && closed.active_instances == 0U);
        assert(released.prepared_ability_slots == 0U && released.prepared_event_slots == 0U);
        std::puts("VALUE_CLEANUP invocation_errors=0 begin=1 end=1 prepared=0 instances=0 PASS");
        return 0;
    }
    {
        Runtime valid{backend, 1, pose_tick};
        assert(dispatchRuntimeHook(*valid.system, valid.hook) == 1 && valid.provider.poses == 1);
        assert(valid.system->failures().empty());
        assert(valid.system->shutdown() && valid.provider.angles == 2); // EndPlay remains authorized.
    }
    {
        Runtime constant{backend, 1, "assert(lux.Values.inspect({id=4,weight=2})==6)"};
        assert(dispatchRuntimeHook(*constant.system, constant.hook) == 1 && constant.system->failures().empty());
    }
    {
        Runtime factory{backend, 1, "assert(lux.Values.token(11,2)==13)"};
        assert(dispatchRuntimeHook(*factory.system, factory.hook) == 1 && factory.provider.tokens == 1);
        assert(ValueToken::live == 0 && ValueToken::release_count == 1 && ValueToken::released[0] == 11);
    }
    {
        Runtime failure{backend, 1, "lux.Values.token(12,'bad')"};
        assert(dispatchRuntimeHook(*failure.system, failure.hook) == 1 && failure.provider.tokens == 0);
        assert(ValueToken::live == 0 && ValueToken::release_count == 2 && ValueToken::released[1] == 12);
    }
    {
        Runtime factory_failure{backend, 1, "lux.Values.token(-1,2)"};
        assert(dispatchRuntimeHook(*factory_failure.system, factory_failure.hook) == 1 &&
            factory_failure.provider.tokens == 0);
        assert(ValueToken::live == 0 && ValueToken::release_count == 2);
    }
    {
        Runtime diagnostic{backend, 1,
            "local ok,e=pcall(lux.Values.token,-2,1); assert(not ok and #e>=255)"};
        assert(dispatchRuntimeHook(*diagnostic.system, diagnostic.hook) == 1 && diagnostic.provider.tokens == 0);
        assert(ValueToken::live == 0 && ValueToken::release_count == 2);
    }
    for (const char* expression : {
        "{key=7,velocity={x=1.5,y=2.25},mode=2}",
        "{key=7,velocity={x='bad',y=2.25},mode=3}",
        "{key=7,velocity={x=1.5,y=2.25},mode=3,extra=1}",
        "{velocity={x=1.5,y=2.25},mode=3}"
    })
    {
        Runtime invalid{backend, 1, std::string{"lux.Values.echo("} + expression + ")"};
        assert(dispatchRuntimeHook(*invalid.system, invalid.hook) == 1);
        assert(invalid.provider.poses == 0 && !invalid.system->failures().empty());
    }
    {
        Runtime output{backend, 1, pose_tick};
        output.provider.invalid_result = true;
        assert(dispatchRuntimeHook(*output.system, output.hook) == 1);
        assert(output.provider.poses == 1 && !output.system->failures().empty());
    }
    {
        Runtime first{backend, 1, "assert(math.abs(lux.Values.angle(90)-90)<0.001)"};
        Runtime second{backend, 2, pose_tick};
        outer = &first; nested = &second; value_reentry = reenter;
        assert(dispatchRuntimeHook(*first.system, first.hook) == 1);
        assert(first.provider.angles == 2 && second.provider.poses == 1 && first.system->failures().empty());
        value_reentry = retire;
        assert(dispatchRuntimeHook(*first.system, first.hook) == 1);
        assert(first.provider.angles == 2 && !first.system->failures().empty());
        assert(executeRuntimeStablePoint(*first.system));
        assert(first.provider.angles == 3); // Only qualified EndPlay, never the invalidated ordinary provider.
    }
    {
        Runtime revoked{backend, 1,
            "assert(not pcall(lux.Values.angle,90)); assert(not pcall(lux.Values.angle,90))"};
        outer = &revoked; value_reentry = retire;
        assert(dispatchRuntimeHook(*revoked.system, revoked.hook) == 1 && revoked.provider.angles == 1);
        assert(executeRuntimeStablePoint(*revoked.system) && revoked.provider.angles == 2);
    }
    {
        Runtime first{backend, 1, "lux.Values.angle(90)"};
        outer = &first; value_reentry = closeDuringRead;
        assert(dispatchRuntimeHook(*first.system, first.hook) == 1);
        assert(first.provider.angles == 2 && first.system->failures().empty());
        assert(first.system->processLifecycle() && first.system->isShutdown() && first.provider.angles == 3);
    }
    for (const auto* name : {"retire-scalar", "retire-zero", "fault-scalar", "fault-zero",
        "stop-scalar", "stop-zero", "input-recovery"})
        admissionCase(backend, name);
    standaloneCase(backend);
    deferredHostCase();
    std::puts("VALUE_RUNTIME generated-pose enum override provider-count lifecycle reentry retire shutdown PASS");
}

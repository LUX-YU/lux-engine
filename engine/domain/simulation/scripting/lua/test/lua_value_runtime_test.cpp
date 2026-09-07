#include "../../../system/test/HookInvocationTestAccess.hpp"
#include "LuaValueTestTypes.lua.value.generated.hpp"
#include "LuaValueTestAbility.ability.generated.hpp"
#include "LuaValueTestAbility.ability.lua.generated.hpp"
#include "LuaPushOnlyAbility.ability.generated.hpp"
#include "LuaPushOnlyAbility.ability.lua.generated.hpp"
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/scripting/ScriptLifecycle.hpp>
#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

using namespace lux::simulation;
using namespace lux::simulation::script;
using namespace lux::simulation::script::test;
using lux::simulation::test::dispatchHookForTest;
using AbilityTraits = lux::script::ScriptAbilityTraits<LuaValueTestAbility>;
constexpr lux::system::SystemInstanceId kOwner{0x5A0101};
constexpr HookPointId kHook{0x5A0102};
constexpr lux::script::ScriptSymbolId kTick{0x5A0103}, kBegin{0x5A0104}, kEnd{0x5A0105};

struct Provider
{
    std::size_t poses{}, angles{}, tokens{};
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
};
static SimulationDescription simulation()
{
    const std::array hooks{makeHookPointSpec<void()>(kHook, "value-tick")};
    const SimulationSystemDescription description{
        .type = {.canonical_name = "lux.test.values", .version = 1}, .hooks = hooks};
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
    SimulationDescription sim = simulation();
    lux::script::ScriptArtifact script;
    lux::asset::AssetId asset;
    ecs::Registry registry;
    ecs::Entity entity = registry.create();
    SimulationClock clock;
    HookPoint<void()> hook;
    std::optional<ScriptHookEndpoint<void()>> endpoint;
    Provider provider;
    decltype(lux::script::bindScriptAbility<LuaValueTestAbility>(provider)) binding;
    std::optional<ScriptSystem> system;
    Runtime(LuaScriptBackend& backend, std::uint8_t id, std::string_view tick)
        : script(artifact(tick)), binding(lux::script::bindScriptAbility<LuaValueTestAbility>(provider))
    {
        std::array<std::uint8_t, 16> bytes{};
        static std::uint8_t incarnation{};
        bytes[0] = id;
        bytes[1] = ++incarnation; // Different source text is a different asset, never a hot reload.
        asset = lux::asset::AssetId{bytes};
        assert(hook.prepare(1) == EEndpointMutationError::NONE);
        endpoint.emplace(kOwner, kHook, hook);
        const std::array mounts{ScriptRuntimeMount{ScriptMountId{1}, asset, EntityScriptScope{entity},
            {{kTick, HookScriptTarget{kOwner, kHook}}}}};
        const auto plan = planScriptRuntimeCapacity(mounts);
        assert(plan);
        const std::array capabilities{publishScriptAbility(binding)};
        const auto descriptor = backend.descriptor();
        const auto ep = endpoint->descriptor();
        auto created = ScriptSystem::create(sim, *plan, mounts, registry, clock,
            {8U, 1U, 4U, 4U, 4U, 4U, 64U, 4U, 4U, 4U, 4U, 4U},
            {this, [](void* context, const lux::asset::AssetId& requested, ResolvedScriptArtifact& output) noexcept {
                auto& self = *static_cast<Runtime*>(context);
                if (requested != self.asset) return false;
                output.artifact = &self.script;
                return true;
            }}, capabilities, std::span{&descriptor, 1}, std::span{&ep, 1}, {});
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
static Runtime* outer;
static Runtime* nested;
static void reenter() noexcept
{
    value_reentry = nullptr;
    const auto previous = nested->provider.poses;
    assert(dispatchHookForTest(nested->hook) == 1);
    assert(nested->provider.poses == previous + 1);
}
static void retire() noexcept
{
    value_reentry = nullptr;
    outer->registry.destroy(outer->entity); // Actual structural invalidation, not a deferred request.
    assert(!outer->registry.valid(outer->entity));
    const auto status = outer->system->queryMountStatus({1});
    assert(!status && status.error() == EScriptSystemError::ENDPOINT_BUSY);
}
static void closeDuringRead() noexcept
{
    value_reentry = nullptr;
    assert(!outer->system->shutdown()); // Existing protection retains resources; new call admission is stopped.
}
static constexpr std::string_view pose_tick =
    "local p=lux.Values.echo({key=7,velocity={x=1.5,y=2.25},mode=3});"
    "assert(p.key==8 and p.velocity.x==3 and p.velocity.y==5.25 and p.mode==3)";
int main(int argc, char** argv)
{
    static_assert(!std::is_default_constructible_v<ValueToken>);
    static_assert(!lux::script::lua::LuaValueCodec<ValuePushOnly>::can_read);
    const auto push_only = lux::script::lua::makeScriptAbilityLuaContribution<LuaPushOnlyAbility>();
    const auto rejected = LuaScriptBackend::create({.instance_capacity = 1, .prepared_call_capacity = 3,
        .continuation_capacity = 1, .execution_depth_capacity = 2, .ability_catalog_method_capacity = 1,
        .abilities = std::span{&push_only, 1}});
    assert(!rejected && rejected.error() == ELuaScriptBindingBackendError::UNSUPPORTED_ABILITY_TYPE);
    const auto contribution = lux::script::lua::makeScriptAbilityLuaContribution<LuaValueTestAbility>();
    const auto policy = argc > 1 && std::string_view{argv[1]} == "--interpreter-only" ?
        lux::script::lua::ELuaExecutionPolicy::INTERPRETER_ONLY : lux::script::lua::ELuaExecutionPolicy::DEFAULT;
    auto created = LuaScriptBackend::create({
        .instance_capacity = 2, .prepared_call_capacity = 64, .continuation_capacity = 2,
        .execution_depth_capacity = 8, .ability_catalog_method_capacity = AbilityTraits::Methods.size(),
        .prepared_ability_capacity = 2 * AbilityTraits::Methods.size(), .abilities = std::span{&contribution, 1},
        .execution_policy = policy, .track_vm_allocations = true,
        .prepared_ability_blocks = std::array{LuaPreparedBlockClass{AbilityTraits::Methods.size(), 2}},
        .prepared_ability_storage_bytes = 1024 * 1024});
    assert(created);
    auto backend = std::move(*created);
    if (argc > 2 && std::string_view{argv[1]} == "--benchmark")
    {
        const auto count = std::strtoull(argv[2], nullptr, 10);
        Runtime runtime{backend, 1, pose_tick};
        for (int i{}; i < 1000; ++i) assert(dispatchHookForTest(runtime.hook) == 1);
        const auto before = backend.stats();
        const auto begin = std::chrono::steady_clock::now();
        for (std::size_t i{}; i < count; ++i) assert(dispatchHookForTest(runtime.hook) == 1);
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - begin).count();
        assert(runtime.provider.poses == count + 1000 && runtime.system->failures().empty());
        const auto after = backend.stats();
        std::printf("VALUE_BENCH count=%llu ns=%lld operations=%zu errors=0 backlog=%zu vm_allocations=%llu\n",
            count, elapsed, runtime.provider.poses - 1000, runtime.system->activeContinuationCount(),
            after.vm_allocations.allocations - before.vm_allocations.allocations);
        return 0;
    }
    {
        Runtime valid{backend, 1, pose_tick};
        assert(dispatchHookForTest(valid.hook) == 1 && valid.provider.poses == 1);
        assert(valid.system->failures().empty());
        assert(valid.system->shutdown() && valid.provider.angles == 2); // EndPlay remains authorized.
    }
    {
        Runtime constant{backend, 1, "assert(lux.Values.inspect({id=4,weight=2})==6)"};
        assert(dispatchHookForTest(constant.hook) == 1 && constant.system->failures().empty());
    }
    {
        Runtime factory{backend, 1, "assert(lux.Values.token(11,2)==13)"};
        assert(dispatchHookForTest(factory.hook) == 1 && factory.provider.tokens == 1);
        assert(ValueToken::live == 0 && ValueToken::release_count == 1 && ValueToken::released[0] == 11);
    }
    {
        Runtime failure{backend, 1, "lux.Values.token(12,'bad')"};
        assert(dispatchHookForTest(failure.hook) == 1 && failure.provider.tokens == 0);
        assert(ValueToken::live == 0 && ValueToken::release_count == 2 && ValueToken::released[1] == 12);
    }
    {
        Runtime factory_failure{backend, 1, "lux.Values.token(-1,2)"};
        assert(dispatchHookForTest(factory_failure.hook) == 1 && factory_failure.provider.tokens == 0);
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
        assert(dispatchHookForTest(invalid.hook) == 1);
        assert(invalid.provider.poses == 0 && !invalid.system->failures().empty());
    }
    {
        Runtime output{backend, 1, pose_tick};
        output.provider.invalid_result = true;
        assert(dispatchHookForTest(output.hook) == 1);
        assert(output.provider.poses == 1 && !output.system->failures().empty());
    }
    {
        Runtime first{backend, 1, "assert(math.abs(lux.Values.angle(90)-90)<0.001)"};
        Runtime second{backend, 2, pose_tick};
        outer = &first; nested = &second; value_reentry = reenter;
        assert(dispatchHookForTest(first.hook) == 1);
        assert(first.provider.angles == 2 && second.provider.poses == 1 && first.system->failures().empty());
        value_reentry = retire;
        assert(dispatchHookForTest(first.hook) == 1);
        assert(first.provider.angles == 2 && !first.system->failures().empty());
        assert(first.system->executeStablePoint());
        assert(first.provider.angles == 3); // Only qualified EndPlay, never the invalidated ordinary provider.
    }
    {
        Runtime revoked{backend, 1,
            "assert(not pcall(lux.Values.angle,90)); assert(not pcall(lux.Values.angle,90))"};
        outer = &revoked; value_reentry = retire;
        assert(dispatchHookForTest(revoked.hook) == 1 && revoked.provider.angles == 1);
        assert(revoked.system->executeStablePoint() && revoked.provider.angles == 2);
    }
    {
        Runtime first{backend, 1, "lux.Values.angle(90)"};
        outer = &first; value_reentry = closeDuringRead;
        assert(dispatchHookForTest(first.hook) == 1);
        assert(first.provider.angles == 1 && !first.system->failures().empty());
        assert(first.system->shutdown() && first.provider.angles == 2);
    }
    std::puts("VALUE_RUNTIME generated-pose enum override provider-count lifecycle reentry retire shutdown PASS");
}

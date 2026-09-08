#include <optional>
#include "Values.lua.value.generated.hpp"
#include "Ability.ability.generated.hpp"
#include "Ability.ability.lua.generated.hpp"
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/scripting/ScriptLifecycle.hpp>
#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>
#include <array>
#include <cassert>
#include <cstdio>
#include <memory>
#include <string_view>

namespace
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;
    using Traits = lux::script::ScriptAbilityTraits<ValueAbility>;
    constexpr lux::system::SystemInstanceId Owner{601};
    constexpr HookPointId Tick{602};
    constexpr lux::script::ScriptSymbolId TickSymbol{603}, BeginSymbol{604}, EndSymbol{605};
    constexpr std::array Hooks{makeHookPointSpec<void()>(Tick, "values", true, true)};

    struct Counts final
    {
        std::size_t constructions{}, destructions{}, calls{}, hooks{}, leases{}, releases{};
        std::array<Item, 4> inputs{};
    };

    struct Provider final
    {
        inline static constexpr auto Access = makeSystemAccessSpec<>();
        inline static constexpr SimulationSystemDescription Description{
            .type = {.canonical_name = "consumer.values.runtime", .version = 1}, .hooks = Hooks
        };
        Counts& counts;
        HookPoint<void()> hook;
        ScriptHookEndpoint<void()> endpoint{Owner, Tick, hook};

        explicit Provider(Counts& value) noexcept : counts(value)
        {
            ++counts.constructions;
            assert(hook.prepare(1) == EEndpointMutationError::NONE);
        }
        ~Provider() { ++counts.destructions; }
        Item echo(const Item& input) noexcept
        {
            assert(counts.calls < counts.inputs.size());
            counts.inputs[counts.calls++] = input;
            return {static_cast<ValueScalar>(input.id * 3 + 3), input.weight * 2 + 4};
        }
    };

    auto install(SimulationBuilder& builder, SimulationSystemView view) noexcept
        -> lux::cxx::expected<void, SimulationSystemBuildFailure>
    {
        auto provider = builder.emplaceSystem<Provider>(view.instanceId(), *builder.registry().ctx().get<Counts*>());
        if (!provider) return lux::cxx::unexpected(provider.error());
        auto result = builder.publishScriptAbility(
            view.instanceId(), lux::script::bindScriptAbility<ValueAbility>(**provider)
        );
        if (!result) return result;
        result = builder.publishScriptHook(view.instanceId(), (*provider)->endpoint.descriptor());
        if (!result) return result;
        return builder.addSystemHookTask<Provider>(view.instanceId(), Tick,
            [](Provider& value, const HookInvocation& invocation) noexcept {
                ++value.counts.hooks;
                assert(value.hook.dispatch(invocation) == 1);
            });
    }

    lux::script::ScriptArtifact makeArtifact()
    {
        lux::rdesc::Script description;
        description.module_name = "consumer.values.runtime";
        description.body = lux::rdesc::LuaSourceScript{"Values"};
        description.api_requirements.push_back({
            lux::script::ScriptApiContractId{Traits::Description.id.name()}, Traits::Description.schema_hash
        });
        description.exports = {{"tick", TickSymbol, {}, {}}, {"begin_life", BeginSymbol, {}, {}},
            {"end_life", EndSymbol, {lux::rdesc::makeScriptValueType<EScriptEndPlayReason>()}, {}}};
        description.lifecycle = {BeginSymbol, EndSymbol};
        constexpr std::string_view Source = R"lua(
return {
    begin_life=function(self)
        self.phase=0
        local x=lux.Values.echo({key=1,weight=0})
        assert(x.key==6 and x.weight==4)
    end,
    tick=function(self)
        self.phase=self.phase+1
        if self.phase==1 then
            local x=lux.Values.echo({key=7,weight=2.5})
            assert(x.key==24 and x.weight==9)
        elseif self.phase==2 then
            local ok,err=pcall(lux.Values.echo,{key=7})
            assert(not ok and type(err)=='string')
        elseif self.phase==3 then
            local ok,err=pcall(lux.Values.echo,{key=7,weight='bad'})
            assert(not ok and type(err)=='string')
        elseif self.phase==4 then
            local x=lux.Values.echo({key=11,weight=1.25})
            assert(x.key==36 and x.weight==6.5)
        else error('unexpected extra Hook') end
    end,
    end_life=function(self,reason)
        assert(self.phase==4)
        local x=lux.Values.echo({key=2,weight=1})
        assert(x.key==9 and x.weight==6)
    end
}
)lua";
        const auto bytes = std::as_bytes(std::span{Source.data(), Source.size()});
        auto result = lux::script::ScriptArtifact::create(std::move(description), {bytes.begin(), bytes.end()});
        assert(result);
        return std::move(*result);
    }
}

void runInstalledRuntime()
{
    Counts counts;
    {
        auto artifact = makeArtifact();
        SimulationDescriptionBuilder builder;
        assert(builder.addSystem(Owner, "values", Provider::Description));
        auto built = std::move(builder).build();
        assert(built);
        auto description = std::make_shared<const SimulationDescription>(std::move(*built));
        ecs::Registry registry;
        registry.ctx().emplace<Counts*>(&counts);
        const auto entity = registry.create();
        SimulationSystemRegistry registrations;
        assert(registrations.add({lux::system::systemTypeId(Provider::Description.type.canonical_name),
            lux::cxx::typeToken<Provider>(), &Provider::Description, Provider::Access.spec(), {}, &install}));
        auto simulation = Simulation::create(registry, description, registrations);
        assert(simulation && counts.constructions == 1);
        const auto contribution = lux::script::lua::makeScriptAbilityLuaContribution<ValueAbility>();
        const std::array blocks{LuaPreparedBlockClass{Traits::Methods.size(), 1}};
        auto backend = LuaScriptBackend::create({
            .instance_capacity = 1, .prepared_call_capacity = 3, .continuation_capacity = 1,
            .execution_depth_capacity = 4, .ability_catalog_method_capacity = Traits::Methods.size(),
            .prepared_ability_capacity = Traits::Methods.size(), .abilities = std::span{&contribution, 1},
            .prepared_ability_blocks = blocks, .prepared_ability_storage_bytes = 65536
        });
        assert(backend);
        std::array<std::uint8_t, 16> bytes{};
        bytes[0] = 61;
        const lux::asset::AssetId asset{bytes};
        struct Assets final { const lux::script::ScriptArtifact& artifact; lux::asset::AssetId id; Counts& counts; };
        Assets assets{artifact, asset, counts};
        const std::array mounts{ScriptRuntimeMount{{1}, asset, EntityScriptScope{entity},
            {{TickSymbol, HookScriptTarget{Owner, Tick}}}}};
        auto capacity = planScriptRuntimeCapacity(mounts);
        assert(capacity);
        const std::array backends{backend->descriptor()};
        auto runtime = ScriptSystem::create(*description, *capacity, mounts, registry, simulation->clock(),
            {8, 1, 2, 2, 2, 2, 64, 2, 2, 2, 2, 2},
            {&assets, [](void* context, const lux::asset::AssetId& id, ResolvedScriptArtifact& output) noexcept {
                auto& value = *static_cast<Assets*>(context);
                if (id != value.id) return false;
                ++value.counts.leases;
                output = {&value.artifact, &value.counts, [](void* lease) noexcept {
                    ++static_cast<Counts*>(lease)->releases;
                }};
                return true;
            }}, simulation->scriptApiCapabilities(), backends,
            simulation->scriptHookEndpoints(), simulation->scriptEventEndpoints());
        assert(runtime && runtime->prepare());
        assert(counts.calls == 1 && counts.inputs[0].id == 1 && counts.inputs[0].weight == 0);
        assert(runtime->activeInstanceCount() == 1 && backend->stats().prepared_ability_slots == 1);
        struct HookContext final
        {
            ScriptSystem& system;
            std::optional<ScriptSystem::ExecutionRegion> region;
        } hook_context{*runtime, {}};
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
        assert(connection);
        auto executor = lux::task::TaskExecutor::create({2, 8});
        assert(executor);
        constexpr std::array ExpectedCalls{2U, 2U, 2U, 3U};
        for (std::size_t phase{}; phase < ExpectedCalls.size(); ++phase)
        {
            assert(simulation->execute(*executor, SimulationDuration{1}));
            assert(counts.hooks == phase + 1 && counts.calls == ExpectedCalls[phase]);
            assert(runtime->failures().empty());
            std::printf("INSTALLED_RUNTIME phase=%zu calls=%zu failures=0 PASS\n", phase + 1, counts.calls);
        }
        assert(counts.inputs[1].id == 7 && counts.inputs[1].weight == 2.5);
        assert(counts.inputs[2].id == 11 && counts.inputs[2].weight == 1.25);
        assert(runtime->shutdown());
        assert(counts.calls == 4 && counts.inputs[3].id == 2 && counts.inputs[3].weight == 1);
        assert(counts.leases == 1 && counts.releases == 1);
        const auto stats = runtime->stats();
        assert(stats.active_instances == 0 && stats.active_continuations == 0 && stats.active_awaitables == 0);
        assert(stats.active_event_waiters == 0 && stats.resume_queue_depth == 0);
        assert(backend->stats().prepared_ability_slots == 0 && runtime->failures().empty());
        assert(runtime->shutdown() && counts.calls == 4 && counts.releases == 1);
    }
    assert(counts.constructions == 1 && counts.destructions == 1);
    assert(counts.calls == 4 && counts.leases == 1 && counts.releases == 1);
    std::puts(
        "INSTALLED_RUNTIME legal=2 rejected=2 begin=1 end=1 provider_ctor=1 provider_dtor=1 lease=1 release=1 backlog=0 PASS"
    );
}

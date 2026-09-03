#include "LuaRuntimeTestAbility.hpp"
#include "LuaRuntimeTestAbility.ability.generated.hpp"

#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/scripting/ScriptLifecycle.hpp>
#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;
    using Ability = test::LuaRuntimeTestAbility;
    using AbilityTraits = lux::script::ScriptAbilityTraits<Ability>;

    inline constexpr lux::system::SystemInstanceId kSystem{0x4C554101U};
    inline constexpr HookPointId kSyncHook{0x4C554102U};
    inline constexpr HookPointId kAsyncHook{0x4C554103U};
    inline constexpr EventPointId kAsyncEvent{0x4C554108U};
    inline constexpr lux::script::ScriptSymbolId kSyncSymbol{0x4C554104U};
    inline constexpr lux::script::ScriptSymbolId kAsyncSymbol{0x4C554105U};
    inline constexpr lux::script::ScriptSymbolId kBeginSymbol{0x4C554106U};
    inline constexpr lux::script::ScriptSymbolId kEndSymbol{0x4C554107U};
    inline constexpr lux::script::ScriptSymbolId kEventSymbol{0x4C554109U};

    struct Provider final
    {
        std::int32_t value{7};
        std::size_t reads{};
        std::size_t writes{};
        bool eager{};
        bool reject{};
        std::optional<lux::script::ScriptAbilityCompletion<std::int32_t>> pending;
        std::array<lux::script::ScriptAbilityCompletion<std::int32_t>, 8U> completions;
        std::size_t completion_count{};
        std::optional<lux::cxx::expected<void, lux::script::EScriptAbilityCompletionError>> eager_result;

        std::int32_t readValue(std::int32_t input) noexcept
        {
            ++reads;
            return value + input;
        }

        void writeValue(std::int32_t next) noexcept
        {
            ++writes;
            value = next;
        }

        const std::int32_t& borrowValue() noexcept
        {
            return value;
        }

        lux::script::ScriptAbilityStartResult beginOperation(
            std::int32_t input,
            lux::script::ScriptAbilityCompletion<std::int32_t> completion
        ) noexcept
        {
            if (reject)
                return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{81});
            if (eager)
            {
                eager_result = completion.success(input + 1);
                return {};
            }
            pending = completion;
            completions[completion_count++] = std::move(completion);
            return {};
        }
    };

    [[nodiscard]] lux::asset::AssetId assetId()
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[0] = 0x4CU;
        return lux::asset::AssetId{bytes};
    }

    [[nodiscard]] lux::world::WorldObjectId objectId()
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[15] = 0x41U;
        return lux::world::WorldObjectId{uuids::uuid{bytes}};
    }

    [[nodiscard]] SimulationDescription makeSimulation()
    {
        constexpr std::array hooks{
            makeHookPointSpec<void()>(kSyncHook, "lua-sync"),
            makeHookPointSpec<void()>(kAsyncHook, "lua-async")
        };
        constexpr std::array events{
            makeEventPointSpec<std::int32_t>(
                kAsyncEvent,
                "lua-event",
                kAsyncHook,
                EEventRoute::SIMULATION_BROADCAST,
                "lux.i32",
                1U
            )
        };
        const SimulationSystemDescription system{
            .type = {.canonical_name = "lux.test.lua-runtime", .version = 1U},
            .hooks = hooks,
            .events = events
        };
        SimulationDescriptionBuilder builder;
        assert(builder.addSystem(kSystem, "lua-runtime", system));
        auto description = std::move(builder).build();
        assert(description);
        return std::move(*description);
    }

    [[nodiscard]] lux::script::ScriptArtifact makeArtifact(bool require_ability = true)
    {
        constexpr std::string_view source = R"lua(
            return {
                begin_life = function(self)
                    self.begun = true
                end,
                end_life = function(self, reason)
                    if not self.begun or reason < 0 then
                        error("invalid lifecycle state")
                    end
                    self.ended = true
                end,
                sync_tick = function(self)
                    if not self.begun then error("BeginPlay did not run") end
                    local value = lux.LuaRuntimeTest.readValue(5)
                    lux.LuaRuntimeTest.writeValue(value)
                    self.sync_count = (self.sync_count or 0) + 1
                end,
                async_tick = function(self)
                    self.async_count = (self.async_count or 0) + 1
                    local value = lux.LuaRuntimeTest.readValue(5)
                    lux.LuaRuntimeTest.writeValue(value)
                    self.borrowed_copy = lux.LuaRuntimeTest.borrowValue()
                    local result = lux.LuaRuntimeTest.beginOperation(self.borrowed_copy)
                    self.async_count = self.async_count + result
                    lux.LuaRuntimeTest.writeValue(self.borrowed_copy + result)
                    local second = lux.LuaRuntimeTest.beginOperation(result)
                    self.async_count = self.async_count + second
                    lux.LuaRuntimeTest.writeValue(self.borrowed_copy + result + second)
                end,
                async_event = function(self, input)
                    self.event_count = (self.event_count or 0) + 1
                    local result = lux.LuaRuntimeTest.beginOperation(input)
                    self.event_count = self.event_count + result
                    lux.LuaRuntimeTest.writeValue(self.event_count)
                end
            }
        )lua";
        lux::rdesc::Script description;
        description.module_name = "lux.test.lua-runtime.fixture";
        description.exports.push_back({"sync_tick", kSyncSymbol, {}, {}});
        description.exports.push_back({"async_tick", kAsyncSymbol, {}, {}});
        description.exports.push_back({
            "async_event",
            kEventSymbol,
            {lux::rdesc::makeScriptValueType<std::int32_t>(lux::semantic::EValuePass::CONST_REF)},
            {}
        });
        description.exports.push_back({"begin_life", kBeginSymbol, {}, {}});
        description.exports.push_back({
            "end_life",
            kEndSymbol,
            {lux::rdesc::makeScriptValueType<EScriptEndPlayReason>()},
            {}
        });
        description.lifecycle = {kBeginSymbol, kEndSymbol};
        if (require_ability)
        {
            description.api_requirements.push_back({
                lux::script::ScriptApiContractId{AbilityTraits::Description.id.name()},
                AbilityTraits::Description.schema_hash
            });
        }
        description.body = lux::rdesc::LuaSourceScript{
            "LuaRuntimeFixture",
            {kAsyncSymbol, kEventSymbol}
        };
        std::vector<std::byte> payload;
        payload.reserve(source.size());
        for (const auto value : source)
            payload.push_back(static_cast<std::byte>(value));
        auto artifact = lux::script::ScriptArtifact::create(std::move(description), std::move(payload));
        assert(artifact);
        return std::move(*artifact);
    }

    [[nodiscard]] lux::script::ScriptArtifact makeRawYieldArtifact()
    {
        constexpr std::string_view source = R"lua(
            return {
                begin_life = function(self) self.begun = true end,
                end_life = function(self, reason) self.ended = reason end,
                sync_tick = function(self) self.sync_count = (self.sync_count or 0) + 1 end,
                async_tick = function(self) coroutine.yield() end,
                async_event = function(self, input) self.event_value = input end
            }
        )lua";
        lux::rdesc::Script description;
        description.module_name = "lux.test.lua-runtime.raw-yield";
        description.exports.push_back({"sync_tick", kSyncSymbol, {}, {}});
        description.exports.push_back({"async_tick", kAsyncSymbol, {}, {}});
        description.exports.push_back({
            "async_event",
            kEventSymbol,
            {lux::rdesc::makeScriptValueType<std::int32_t>(lux::semantic::EValuePass::CONST_REF)},
            {}
        });
        description.exports.push_back({"begin_life", kBeginSymbol, {}, {}});
        description.exports.push_back({
            "end_life",
            kEndSymbol,
            {lux::rdesc::makeScriptValueType<EScriptEndPlayReason>()},
            {}
        });
        description.lifecycle = {kBeginSymbol, kEndSymbol};
        description.body = lux::rdesc::LuaSourceScript{"LuaRuntimeRawYield", {kAsyncSymbol}};
        std::vector<std::byte> payload;
        payload.reserve(source.size());
        for (const auto value : source)
            payload.push_back(static_cast<std::byte>(value));
        auto artifact = lux::script::ScriptArtifact::create(std::move(description), std::move(payload));
        assert(artifact);
        return std::move(*artifact);
    }

    struct Harness final
    {
        explicit Harness(
            bool require_ability = true,
            std::size_t lua_continuation_capacity = 4U
        )
            : simulation(makeSimulation()),
              artifact(makeArtifact(require_ability)),
              asset(assetId()),
              object(objectId())
        {
            entity = registry.create();
            ScriptSystemDescriptionBuilder builder;
            assert(builder.addMount({
                ScriptMountId{1U},
                asset,
                EntityScriptMount{object},
                true,
                {
                    {kSyncSymbol, HookScriptTarget{kSystem, kSyncHook}},
                    {kAsyncSymbol, HookScriptTarget{kSystem, kAsyncHook}},
                    {kEventSymbol, EventScriptTarget{kSystem, kAsyncEvent}}
                }
            }));
            auto built = std::move(builder).build(simulation);
            assert(built);
            description = std::move(*built);
            assert(sync_hook.prepare(1U) == EEndpointMutationError::NONE);
            assert(async_hook.prepare(1U) == EEndpointMutationError::NONE);
            assert(async_event.prepare(1U, 4U, 4U) == EEndpointMutationError::NONE);
            sync_endpoint.emplace(kSystem, kSyncHook, sync_hook);
            async_endpoint.emplace(kSystem, kAsyncHook, async_hook);
            event_endpoint.emplace(kSystem, kAsyncEvent, async_event);
            endpoints = {sync_endpoint->descriptor(), async_endpoint->descriptor()};
            event_endpoints = {event_endpoint->descriptor()};
            contribution = lux::script::lua::makeScriptAbilityLuaContribution<Ability>();
            auto created_backend = LuaScriptBackend::create({
                1U,
                8U,
                lua_continuation_capacity,
                8U,
                AbilityTraits::Description.methods.size(),
                {},
                {},
                std::span{&contribution, 1U}
            });
            assert(created_backend);
            backend.emplace(std::move(*created_backend));
            descriptor = backend->descriptor();
        }

        [[nodiscard]] lux::cxx::expected<ScriptSystem, EScriptSystemError> create(
            std::span<const ScriptApiCapabilityPublication> capabilities
        ) noexcept
        {
            return ScriptSystem::create(
                simulation,
                description,
                registry,
                clock,
                {16U, 1U, 4U, 4U, 4U, 4U, 64U, 4U, 4U, 4U},
                {this, &resolveArtifact},
                {this, &resolveWorld},
                capabilities,
                std::span{&descriptor, 1U},
                endpoints,
                event_endpoints
            );
        }

        static bool resolveArtifact(
            void* context,
            const lux::asset::AssetId& requested,
            ResolvedScriptArtifact& result
        ) noexcept
        {
            auto& self = *static_cast<Harness*>(context);
            if (requested != self.asset)
                return false;
            result.artifact = std::addressof(self.artifact);
            return true;
        }

        static bool resolveWorld(
            void* context,
            const lux::world::WorldObjectId& requested,
            ecs::Entity& result
        ) noexcept
        {
            auto& self = *static_cast<Harness*>(context);
            if (requested != self.object)
                return false;
            result = self.entity;
            return true;
        }

        SimulationDescription simulation;
        lux::script::ScriptArtifact artifact;
        lux::asset::AssetId asset;
        lux::world::WorldObjectId object;
        ecs::Registry registry;
        ecs::Entity entity{ecs::NullEntity};
        SimulationClock clock;
        ScriptSystemDescription description;
        HookPoint<void()> sync_hook;
        HookPoint<void()> async_hook;
        EventPoint<SimulationBroadcastRoute, std::int32_t> async_event;
        std::optional<ScriptHookEndpoint<void()>> sync_endpoint;
        std::optional<ScriptHookEndpoint<void()>> async_endpoint;
        std::optional<ScriptEventEndpoint<SimulationBroadcastRoute, std::int32_t>> event_endpoint;
        std::array<ScriptHookEndpointDescriptor, 2U> endpoints;
        std::array<ScriptEventEndpointDescriptor, 1U> event_endpoints;
        lux::script::lua::ScriptAbilityLuaContribution contribution;
        std::optional<LuaScriptBackend> backend;
        ScriptBackendDescriptor descriptor;
    };
} // namespace

int main()
{
    Provider provider;
    const auto binding = lux::script::bindScriptAbility<Ability>(provider);
    const std::array capabilities{publishScriptAbility(binding)};
    Harness harness;
    auto created = harness.create(capabilities);
    assert(created);
    auto system = std::move(*created);
    assert(system.prepare());

    assert(harness.sync_hook.dispatch() == 1U);
    assert(provider.reads == 1U && provider.writes == 1U && provider.value == 12);
    assert(system.activeContinuationCount() == 0U);
    assert(system.activeAwaitableCount() == 0U);

    provider.value = 7;
    assert(harness.async_hook.dispatch() == 1U);
    assert(provider.pending.has_value());
    assert(provider.reads == 2U && provider.writes == 2U && provider.value == 12);
    assert(system.activeContinuationCount() == 1U);
    assert(system.activeAwaitableCount() == 1U);
    assert(harness.async_hook.dispatch() == 1U);
    assert(provider.reads == 2U);

    provider.value = 100;
    assert(provider.pending->success(7));
    assert(provider.value == 100);
    assert(system.executeStablePoint());
    assert(provider.value == 19);
    assert(provider.writes == 3U);
    assert(provider.pending->success(3));
    assert(system.executeStablePoint());
    assert(provider.value == 22);
    assert(provider.writes == 4U);
    assert(system.activeContinuationCount() == 0U);
    assert(system.activeAwaitableCount() == 0U);
    assert(system.shutdown());

    Provider eager_provider;
    eager_provider.eager = true;
    const auto eager_binding = lux::script::bindScriptAbility<Ability>(eager_provider);
    const std::array eager_capabilities{publishScriptAbility(eager_binding)};
    Harness eager;
    auto eager_created = eager.create(eager_capabilities);
    assert(eager_created);
    auto eager_system = std::move(*eager_created);
    assert(eager_system.prepare());
    assert(eager.async_hook.dispatch() == 1U);
    assert(eager_provider.value == 12);
    assert(eager_provider.eager_result.has_value() && *eager_provider.eager_result);
    assert(eager_system.activeContinuationCount() == 1U);
    assert(eager_system.executeStablePoint());
    assert(eager_provider.value == 39);
    assert(eager_system.activeContinuationCount() == 0U);
    assert(eager_system.shutdown());

    Provider event_provider;
    const auto event_binding = lux::script::bindScriptAbility<Ability>(event_provider);
    const std::array event_capabilities{publishScriptAbility(event_binding)};
    Harness events;
    auto event_created = events.create(event_capabilities);
    assert(event_created);
    auto event_system = std::move(*event_created);
    assert(event_system.prepare());
    {
        auto writer = events.async_event.begin(0U);
        assert(writer.record(2));
        assert(writer.record(3));
    }
    assert(events.async_event.drain() == 2U);
    assert(event_provider.completion_count == 2U);
    assert(event_system.activeContinuationCount() == 2U);
    assert(event_provider.completions[0].success(10));
    assert(event_provider.completions[1].success(20));
    assert(event_system.executeStablePoint());
    assert(event_provider.value == 32);
    assert(event_system.activeContinuationCount() == 0U);
    assert(event_system.shutdown());

    Provider limited_provider;
    const auto limited_binding = lux::script::bindScriptAbility<Ability>(limited_provider);
    const std::array limited_capabilities{publishScriptAbility(limited_binding)};
    Harness limited{true, 1U};
    auto limited_created = limited.create(limited_capabilities);
    assert(limited_created);
    auto limited_system = std::move(*limited_created);
    assert(limited_system.prepare());
    {
        auto writer = limited.async_event.begin(0U);
        assert(writer.record(2));
        assert(writer.record(3));
    }
    assert(limited.async_event.drain() == 2U);
    assert(!limited_system.failures().empty());
    assert(limited_system.activeContinuationCount() <= 1U);
    assert(limited_system.shutdown());

    Provider retiring_provider;
    const auto retiring_binding = lux::script::bindScriptAbility<Ability>(retiring_provider);
    const std::array retiring_capabilities{publishScriptAbility(retiring_binding)};
    Harness retiring;
    auto retiring_created = retiring.create(retiring_capabilities);
    assert(retiring_created);
    auto retiring_system = std::move(*retiring_created);
    assert(retiring_system.prepare());
    assert(retiring.async_hook.dispatch() == 1U);
    assert(retiring_provider.pending.has_value());
    const auto late_completion = *retiring_provider.pending;
    retiring.registry.destroy(retiring.entity);
    const auto retired = retiring_system.executeStablePoint();
    assert(!retired && retired.error() == EScriptSystemError::WORLD_OBJECT_NOT_RESOLVED);
    assert(retiring_system.activeContinuationCount() == 0U);
    const auto late = late_completion.success(9);
    assert(!late && late.error() == lux::script::EScriptAbilityCompletionError::STALE);
    assert(retiring_system.shutdown());

    Provider failed_provider;
    const auto failed_binding = lux::script::bindScriptAbility<Ability>(failed_provider);
    const std::array failed_capabilities{publishScriptAbility(failed_binding)};
    Harness failed;
    auto failed_created = failed.create(failed_capabilities);
    assert(failed_created);
    auto failed_system = std::move(*failed_created);
    assert(failed_system.prepare());
    assert(failed.async_hook.dispatch() == 1U);
    assert(failed_provider.pending.has_value());
    assert(failed_provider.pending->fail({91}));
    const auto failed_stable = failed_system.executeStablePoint();
    assert(!failed_stable && failed_stable.error() == EScriptSystemError::INVOCATION_FAILURE);
    assert(failed_system.failures().back().status == 91);
    assert(failed_system.activeContinuationCount() == 0U);
    assert(failed_system.shutdown());

    Provider rejected_provider;
    rejected_provider.reject = true;
    const auto rejected_binding = lux::script::bindScriptAbility<Ability>(rejected_provider);
    const std::array rejected_capabilities{publishScriptAbility(rejected_binding)};
    Harness rejected;
    auto rejected_created = rejected.create(rejected_capabilities);
    assert(rejected_created);
    auto rejected_system = std::move(*rejected_created);
    assert(rejected_system.prepare());
    assert(rejected.async_hook.dispatch() == 1U);
    assert(!rejected_system.failures().empty());
    assert(rejected_system.failures().back().status == 81);
    assert(rejected_system.activeContinuationCount() == 0U);
    assert(rejected_system.activeAwaitableCount() == 0U);
    assert(rejected_system.shutdown());

    Harness raw_yield{false};
    raw_yield.artifact = makeRawYieldArtifact();
    auto raw_yield_created = raw_yield.create({});
    assert(raw_yield_created);
    auto raw_yield_system = std::move(*raw_yield_created);
    assert(raw_yield_system.prepare());
    assert(raw_yield.async_hook.dispatch() == 1U);
    assert(!raw_yield_system.failures().empty());
    assert(raw_yield_system.activeContinuationCount() == 0U);
    assert(raw_yield_system.activeAwaitableCount() == 0U);
    assert(raw_yield_system.shutdown());

    Harness undeclared{false};
    auto undeclared_created = undeclared.create({});
    assert(undeclared_created);
    auto undeclared_system = std::move(*undeclared_created);
    assert(undeclared_system.prepare());
    assert(undeclared.sync_hook.dispatch() == 1U);
    assert(!undeclared_system.failures().empty());
    assert(undeclared_system.failures().front().error == EScriptSystemError::INVOCATION_FAILURE);
    assert(undeclared_system.shutdown());
    return 0;
}

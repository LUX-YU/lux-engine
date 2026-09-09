#include "../../../builtin/script/test/ScriptRuntimeTestRegion.hpp"
using lux::simulation::script::test::dispatchRuntimeHook;
using lux::simulation::script::test::deliverRuntimeEvent;
using lux::simulation::script::test::executeRuntimeStablePoint;
#include "../../../system/test/HookInvocationTestAccess.hpp"
using lux::simulation::test::dispatchHookForTest;
#include "../../core/test/ScriptEndpointTestAccess.hpp"
using lux::simulation::script::test::deliverEndpoint;
#include "LuaRuntimeTestAbility.hpp"
#include "LuaRuntimeTestAbility.ability.generated.hpp"
#include "LuaRuntimeTestAbility.ability.lua.generated.hpp"

#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/scripting/ScriptLifecycle.hpp>
#include <lux/engine/simulation/scripting/ScriptEventSource.hpp>
#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <lua.hpp>
#include <cstdio>
#include <cstdlib>

namespace
{
    lux::script::lua::ELuaExecutionPolicy g_execution_policy{
        lux::script::lua::ELuaExecutionPolicy::DEFAULT
    };
    using namespace lux::simulation;
    using namespace lux::simulation::script;
    using Ability = lux::simulation::script::test::LuaRuntimeTestAbility;
    using AbilityTraits = lux::script::ScriptAbilityTraits<Ability>;

    lua_State* g_observed_vm{};
    std::size_t g_observed_read_entries{};
    int (*g_original_read)(lua_State*) noexcept{};
    int observeRead(lua_State* state) noexcept
    {
        g_observed_vm = state;
        ++g_observed_read_entries;
        return g_original_read(state);
    }

    inline constexpr lux::system::SystemInstanceId kSystem{0x4C554101U};
    inline constexpr HookPointId kSyncHook{0x4C554102U};
    inline constexpr HookPointId kAsyncHook{0x4C554103U};
    inline constexpr HookPointId kScalarHook{0x4C55410AU};
    inline constexpr HookPointId kEventWaitHook{0x4C55410CU};
    inline constexpr HookPointId kTargetWaitHook{0x4C55410EU};
    inline constexpr EventPointId kAsyncEvent{0x4C554108U};
    inline constexpr EventPointId kTargetEvent{0x4C55410FU};
    inline constexpr lux::script::ScriptSymbolId kSyncSymbol{0x4C554104U};
    inline constexpr lux::script::ScriptSymbolId kAsyncSymbol{0x4C554105U};
    inline constexpr lux::script::ScriptSymbolId kBeginSymbol{0x4C554106U};
    inline constexpr lux::script::ScriptSymbolId kEndSymbol{0x4C554107U};
    inline constexpr lux::script::ScriptSymbolId kEventSymbol{0x4C554109U};
    inline constexpr lux::script::ScriptSymbolId kScalarSymbol{0x4C55410BU};
    inline constexpr lux::script::ScriptSymbolId kEventWaitSymbol{0x4C55410DU};
    inline constexpr lux::script::ScriptSymbolId kTargetWaitSymbol{0x4C554110U};

    decltype(ScriptBackendDescriptor{}.prepareMethod) g_original_prepare{};
    decltype(BoundScriptStepCall{}.invoke) g_original_step{};
    ScriptStepResult g_observed_step;
    std::size_t g_observed_steps{};
    EScriptBackendResult observePreparedStep(void* context, ScriptBackendInstance instance,
        const lux::rdesc::ScriptFunction& function, ScriptBackendPreparedMethod& result) noexcept
    {
        const auto status = g_original_prepare(context, instance, function, result);
        if (status == EScriptBackendResult::SUCCESS && function.symbol_id == kAsyncSymbol)
        {
            g_original_step = result.resumable.invoke;
            result.resumable.invoke = [](void* opaque, lux_script_call_frame& frame, ScriptStepContext& step,
                ScriptBackendContinuation& continuation) noexcept {
                ++g_observed_steps;
                g_observed_step = g_original_step(opaque, frame, step, continuation);
                return g_observed_step;
            };
        }
        return status;
    }

    SimulationDescription makeSimulation();

    [[nodiscard]] lux::script::ScriptEventSourceDescription eventSource(
        EventPointId event,
        std::string_view name,
        lux::script::EScriptEventRoute route
    )
    {
        const auto simulation = makeSimulation();
        const auto hook = simulation.findEvent(kSystem, event).dispatchHook();
        return {
            "Gameplay",
            std::string(name),
            kSystem.value,
            event.value,
            route,
            {"lux.i32", lux::semantic::typeId("lux.i32"), LUX_SCRIPT_VK_INT32, 4U, 4U},
            lux::semantic::typeId("lux.i32"),
            1U, hook.id().value, hook.contractHash(), hook.contractVersion()
        };
    }

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
        bool bool_value{};
        std::int32_t i32_value{};
        std::uint32_t u32_value{};
        float f32_value{};
        double f64_value{};
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

        bool echoBool(bool input) noexcept
        {
            bool_value = input;
            return input;
        }

        std::int32_t echoI32(std::int32_t input) noexcept
        {
            i32_value = input;
            return input;
        }

        std::uint32_t echoU32(std::uint32_t input) noexcept
        {
            u32_value = input;
            return input;
        }

        float echoF32(float input) noexcept
        {
            f32_value = input;
            return input;
        }

        double echoF64(double input) noexcept
        {
            f64_value = input;
            return input;
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



    [[nodiscard]] SimulationDescription makeSimulation()
    {
        constexpr std::array hooks{
            makeHookPointSpec<void()>(kSyncHook, "lua-sync"),
            makeHookPointSpec<void()>(kAsyncHook, "lua-async"),
            makeHookPointSpec<void()>(kScalarHook, "lua-scalars"),
            makeHookPointSpec<void()>(kEventWaitHook, "lua-event-wait"),
            makeHookPointSpec<void()>(kTargetWaitHook, "lua-target-wait")
        };
        constexpr std::array events{
            makeEventPointSpec<std::int32_t>(
                kAsyncEvent,
                "lua-event",
                kAsyncHook,
                EEventRoute::SIMULATION_BROADCAST,
                "lux.i32",
                1U
            ),
            makeEventPointSpec<std::int32_t>(
                kTargetEvent,
                "lua-targeted-event",
                kTargetWaitHook,
                EEventRoute::ENTITY_TARGETED,
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

    [[nodiscard]] lux::script::ScriptArtifact makeArtifact(
        bool require_ability = true,
        bool declare_event = true
    )
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
                scalar_round_trip = function(self)
                    if lux.LuaRuntimeTest.echoBool(false) ~= false then error("bool mismatch") end
                    if lux.LuaRuntimeTest.echoBool(true) ~= true then error("bool mismatch") end
                    if lux.LuaRuntimeTest.echoI32(-2147483648) ~= -2147483648 then error("i32 min mismatch") end
                    if lux.LuaRuntimeTest.echoI32(2147483647) ~= 2147483647 then error("i32 max mismatch") end
                    if lux.LuaRuntimeTest.echoU32(0) ~= 0 then error("u32 zero mismatch") end
                    if lux.LuaRuntimeTest.echoU32(4294967295) ~= 4294967295 then error("u32 max mismatch") end
                    if lux.LuaRuntimeTest.echoF32(-12.5) ~= -12.5 then error("f32 mismatch") end
                    if lux.LuaRuntimeTest.echoF64(1234.125) ~= 1234.125 then error("f64 mismatch") end
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
                end,
                wait_event = function(self)
                    local payload = lux.Event.Gameplay.damage()
                    self.waited_payload = payload
                    lux.LuaRuntimeTest.writeValue(payload)
                end,
                wait_targeted = function(self)
                    local payload = lux.Event.Gameplay.targeted()
                    self.targeted_payload = payload
                    lux.LuaRuntimeTest.writeValue(payload)
                end
            }
        )lua";
        lux::rdesc::Script description;
        description.module_name = "lux.test.lua-runtime.fixture";
        description.exports.push_back({"sync_tick", kSyncSymbol, {}, {}});
        description.exports.push_back({"scalar_round_trip", kScalarSymbol, {}, {}});
        description.exports.push_back({"async_tick", kAsyncSymbol, {}, {}});
        description.exports.push_back({
            "async_event",
            kEventSymbol,
            {lux::rdesc::makeScriptValueType<std::int32_t>(lux::semantic::EValuePass::CONST_REF)},
            {}
        });
        description.exports.push_back({"begin_life", kBeginSymbol, {}, {}});
        description.exports.push_back({"wait_event", kEventWaitSymbol, {}, {}});
        description.exports.push_back({"wait_targeted", kTargetWaitSymbol, {}, {}});
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
        lux::rdesc::LuaSourceScript lua{
            "LuaRuntimeFixture",
            {kAsyncSymbol, kEventSymbol, kEventWaitSymbol, kTargetWaitSymbol}
        };
        if (declare_event)
        {
            description.event_requirements.push_back(eventSource(
                kAsyncEvent,
                "damage",
                lux::script::EScriptEventRoute::SIMULATION_BROADCAST
            ));
            description.event_requirements.push_back(eventSource(
                kTargetEvent,
                "targeted",
                lux::script::EScriptEventRoute::ENTITY_TARGETED
            ));
        }
        description.body = std::move(lua);
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
                scalar_round_trip = function(self) end,
                async_tick = function(self) coroutine.yield() end,
                async_event = function(self, input) self.event_value = input end,
                wait_event = function(self) end,
                wait_targeted = function(self) end
            }
        )lua";
        lux::rdesc::Script description;
        description.module_name = "lux.test.lua-runtime.raw-yield";
        description.exports.push_back({"sync_tick", kSyncSymbol, {}, {}});
        description.exports.push_back({"scalar_round_trip", kScalarSymbol, {}, {}});
        description.exports.push_back({"async_tick", kAsyncSymbol, {}, {}});
        description.exports.push_back({
            "async_event",
            kEventSymbol,
            {lux::rdesc::makeScriptValueType<std::int32_t>(lux::semantic::EValuePass::CONST_REF)},
            {}
        });
        description.exports.push_back({"begin_life", kBeginSymbol, {}, {}});
        description.exports.push_back({"wait_event", kEventWaitSymbol, {}, {}});
        description.exports.push_back({"wait_targeted", kTargetWaitSymbol, {}, {}});
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
            std::size_t lua_continuation_capacity = 4U,
            bool declare_event = true,
            bool observe_vm = false
        )
            : simulation(makeSimulation()),
              artifact(makeArtifact(require_ability, declare_event)),
              asset(assetId())
        {
            entity = registry.create();
            std::vector<ScriptRuntimeMount> builder;
            builder.push_back({ScriptMountId{1U}, asset, EntityScriptScope{entity}, {
                    {kSyncSymbol, HookScriptTarget{kSystem, kSyncHook}},
                    {kAsyncSymbol, HookScriptTarget{kSystem, kAsyncHook}},
                    {kScalarSymbol, HookScriptTarget{kSystem, kScalarHook}},
                    {kEventWaitSymbol, HookScriptTarget{kSystem, kEventWaitHook}},
                    {kTargetWaitSymbol, HookScriptTarget{kSystem, kTargetWaitHook}},
                    {kEventSymbol, EventScriptTarget{kSystem, kAsyncEvent}}
                }});
            auto built = std::optional{std::move(builder)};
            assert(built);
            description = std::move(*built);
            assert(sync_hook.prepare(1U) == EEndpointMutationError::NONE);
            assert(async_hook.prepare(1U) == EEndpointMutationError::NONE);
            assert(scalar_hook.prepare(1U) == EEndpointMutationError::NONE);
            assert(event_wait_hook.prepare(1U) == EEndpointMutationError::NONE);
            assert(target_wait_hook.prepare(1U) == EEndpointMutationError::NONE);
            assert(async_event.prepare({1U, 4U}) == EEndpointMutationError::NONE);
            assert(target_event.prepare({1U, 4U}) == EEndpointMutationError::NONE);
            sync_endpoint.emplace(kSystem, kSyncHook, sync_hook);
            async_endpoint.emplace(kSystem, kAsyncHook, async_hook);
            scalar_endpoint.emplace(kSystem, kScalarHook, scalar_hook);
            event_wait_endpoint.emplace(kSystem, kEventWaitHook, event_wait_hook);
            target_wait_endpoint.emplace(kSystem, kTargetWaitHook, target_wait_hook);
            event_endpoint.emplace(kSystem, kAsyncEvent, async_event);
            target_event_endpoint.emplace(kSystem, kTargetEvent, target_event);
            endpoints = {
                sync_endpoint->descriptor(),
                async_endpoint->descriptor(),
                scalar_endpoint->descriptor(),
                event_wait_endpoint->descriptor(),
                target_wait_endpoint->descriptor()
            };
            event_endpoints = {event_endpoint->descriptor(), target_event_endpoint->descriptor()};
            auto projected = projectScriptEventSource(
                simulation.findEvent(kSystem, kAsyncEvent),
                event_endpoints.front(),
                "Gameplay",
                "damage"
            );
            assert(projected);
            event_sources[0] = std::move(*projected);
            projected = projectScriptEventSource(
                simulation.findEvent(kSystem, kTargetEvent),
                event_endpoints[1],
                "Gameplay",
                "targeted"
            );
            assert(projected);
            event_sources[1] = std::move(*projected);
            contribution = lux::script::lua::makeScriptAbilityLuaContribution<Ability>();
            if (observe_vm)
            {
                observed_methods.assign(contribution.methods.begin(), contribution.methods.end());
                for (auto& method : observed_methods)
                {
                    if (method.method.name() == "lux.test.lua_runtime.read")
                    {
                        g_original_read = method.entry;
                        method.entry = &observeRead;
                    }
                }
                assert(g_original_read);
                contribution.methods = observed_methods;
            }
            auto created_backend = LuaScriptBackend::create({
                .instance_capacity = 1U,
                .prepared_call_capacity = 16U,
                .continuation_capacity = lua_continuation_capacity,
                .execution_depth_capacity = 8U,
                .ability_catalog_method_capacity = AbilityTraits::Description.methods.size(),
                .prepared_ability_capacity = AbilityTraits::Description.methods.size(),
                .abilities = std::span{&contribution, 1U},
                .execution_policy = g_execution_policy,
                .event_catalog_capacity = event_sources.size(),
                .prepared_event_capacity = event_sources.size(),
                .events = event_sources,
                .track_vm_allocations = true,
                .prepared_ability_blocks = std::array{
                    lux::simulation::script::LuaPreparedBlockClass{
                        (AbilityTraits::Description.methods.size()) / ((1U) == 0U ? 1U : (1U)),
                        1U
                    }
                },
                .prepared_ability_storage_bytes =
                    64U * 1024U * 1024U,
                .prepared_event_blocks = std::array{
                    lux::simulation::script::LuaPreparedBlockClass{
                        (event_sources.size()) / ((1U) == 0U ? 1U : (1U)),
                        1U
                    }
                },
                .prepared_event_storage_bytes =
                    64U * 1024U * 1024U
            });
            assert(created_backend);
            backend.emplace(std::move(*created_backend));
            const auto runtime = backend->runtimeInfo();
            assert(!runtime.vm.empty() && !runtime.version.empty());
            assert(g_execution_policy != lux::script::lua::ELuaExecutionPolicy::INTERPRETER_ONLY ||
                !runtime.jit_enabled);
            descriptor = backend->descriptor();
            if (observe_vm)
            {
                g_original_prepare = descriptor.prepareMethod;
                descriptor.prepareMethod = &observePreparedStep;
                g_observed_steps = 0U;
            }
        }

        [[nodiscard]] lux::cxx::expected<ScriptSystem, EScriptSystemError> create(
            std::span<const ScriptApiCapabilityPublication> capabilities
        ) noexcept
        {
            return ScriptSystem::create(
                simulation,
                *planScriptRuntimeCapacity(description),
                description,
                registry,
                clock,
                {16U, 1U, 4U, 4U, 4U, 4U, 64U, 4U, 4U, 4U, 4U, 4U},
                {this, &resolveArtifact},
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



        SimulationDescription simulation;
        lux::script::ScriptArtifact artifact;
        lux::asset::AssetId asset;
        ecs::Registry registry;
        ecs::Entity entity{ecs::NullEntity};
        SimulationClock clock;
        std::vector<ScriptRuntimeMount> description;
        HookPoint<void()> sync_hook;
        HookPoint<void()> async_hook;
        HookPoint<void()> scalar_hook;
        HookPoint<void()> event_wait_hook;
        HookPoint<void()> target_wait_hook;
        HookChannel<SimulationBroadcastRoute, std::int32_t> async_event;
        HookChannel<EntityTargetedRoute<ecs::Entity>, std::int32_t> target_event;
        std::optional<ScriptHookEndpoint<void()>> sync_endpoint;
        std::optional<ScriptHookEndpoint<void()>> async_endpoint;
        std::optional<ScriptHookEndpoint<void()>> scalar_endpoint;
        std::optional<ScriptHookEndpoint<void()>> event_wait_endpoint;
        std::optional<ScriptHookEndpoint<void()>> target_wait_endpoint;
        std::optional<ScriptEventEndpoint<SimulationBroadcastRoute, std::int32_t>> event_endpoint;
        std::optional<ScriptEventEndpoint<EntityTargetedRoute<ecs::Entity>, std::int32_t>> target_event_endpoint;
        std::array<ScriptHookEndpointDescriptor, 5U> endpoints;
        std::array<ScriptEventEndpointDescriptor, 2U> event_endpoints;
        lux::script::lua::ScriptAbilityLuaContribution contribution;
        std::vector<lux::script::lua::ScriptAbilityLuaMethodProjection> observed_methods;
        std::array<lux::script::ScriptEventSourceDescription, 2U> event_sources;
        std::optional<LuaScriptBackend> backend;
        ScriptBackendDescriptor descriptor;
    };

    struct CreationAllocator final
    {
        lua_Alloc original{};
        void* context{};
        std::size_t permitted{};
        std::size_t growths{}, failures{};
        bool armed{};
        void* observation_context{};
        void (*observe_growth)(void*) noexcept{};
        static void* allocate(void* opaque, void* pointer, std::size_t old_size, std::size_t size) noexcept
        {
            auto& self = *static_cast<CreationAllocator*>(opaque);
            if (self.armed && size != 0U && (pointer == nullptr || size > old_size))
            {
                if (self.observe_growth)
                {
                    const auto callback = self.observe_growth;
                    self.observe_growth = nullptr;
                    callback(self.observation_context);
                }
                const auto index = self.growths++;
                std::printf("CREATE_ALLOCATION,index=%zu,old=%zu,new=%zu,kind=%s\n",
                    index, old_size, size, pointer ? "grow" : "new");
                std::fflush(stdout);
                if (index >= self.permitted)
                {
                    ++self.failures;
                    return nullptr;
                }
            }
            return self.original(self.context, pointer, old_size, size);
        }
    };

    int testCreationOom(std::size_t permitted, bool registry_growth = false)
    {
        Harness harness{true, 1U, true, true};
        Provider provider;
        const auto binding = lux::script::bindScriptAbility<Ability>(provider);
        const std::array publications{publishScriptAbility(binding)};
        auto created = harness.create(publications);
        assert(created);
        auto system = std::move(*created);
        assert(system.prepare());
        assert(dispatchRuntimeHook(system, harness.sync_hook) == 1U);
        assert(g_observed_vm && provider.reads == 1U && provider.writes == 1U);
        auto* vm = g_observed_vm;
        const auto base = lua_gettop(vm);
        std::vector<int> padding;
        if (registry_growth)
        {
            // Force the next registry reference across a public integer-key boundary. Keep all
            // references alive so the upcoming luaL_ref cannot reuse a free-list entry.
            padding.reserve(1024U);
            do
            {
                lua_pushboolean(vm, true);
                padding.push_back(luaL_ref(vm, LUA_REGISTRYINDEX));
                assert(padding.back() > 0 && padding.size() <= 1024U);
            } while (padding.back() < 1024);
            assert(padding.back() == 1024);
        }
        CreationAllocator allocation;
        allocation.original = lua_getallocf(vm, &allocation.context);
        allocation.permitted = permitted;
        const auto panic = lua_atpanic(vm, [](lua_State*) -> int {
            std::puts("UNPROTECTED_CREATION_PANIC,provider_calls_before=2,thread_roots_before=0");
            std::fflush(stdout);
            std::_Exit(86);
        });
        lua_setallocf(vm, &CreationAllocator::allocate, &allocation);
        allocation.armed = true;
        std::puts("CASE,creation-oom,prepared=1,continuation_capacity=1");
        std::fflush(stdout);
        assert(dispatchRuntimeHook(system, harness.async_hook) == 1U);
        allocation.armed = false;
        lua_setallocf(vm, allocation.original, allocation.context);
        lua_atpanic(vm, panic);
        if (registry_growth)
        {
            lua_rawgeti(vm, LUA_REGISTRYINDEX, 1025);
            assert(lua_isnil(vm, -1));
            lua_pop(vm, 1);
        }
        for (const auto reference : padding) luaL_unref(vm, LUA_REGISTRYINDEX, reference);
        assert(allocation.failures != 0U);
        assert(lua_gettop(vm) == base);
        assert(provider.reads == 1U && provider.writes == 1U && !provider.pending);
        assert(system.activeContinuationCount() == 0U && system.activeAwaitableCount() == 0U);
        assert(system.stats().invocation_failures == 1U);
        assert(g_observed_steps == 1U && g_observed_step.state == EScriptStepState::FAILED);
        assert(g_observed_step.error.status == -10);
        assert(system.failures().size() == 1U && system.failures().front().status == -10);
        assert(system.shutdown());
        const auto after = harness.backend->stats();
        assert(after.vm_coroutine_creations == after.vm_coroutine_releases);
        assert(after.prepared_ability_slots == 0U && after.prepared_event_slots == 0U);
        // Reuse the same backend's only continuation slot with a fresh runtime identity.
        harness.entity = harness.registry.create();
        harness.description.front().scope = EntityScriptScope{harness.entity};
        auto next = harness.create(publications);
        assert(next && next->prepare());
        assert(dispatchRuntimeHook(*next, harness.async_hook) == 1U);
        assert(provider.pending && provider.reads == 2U && provider.writes == 2U);
        assert(next->activeContinuationCount() == 1U && next->activeAwaitableCount() == 1U);
        assert(next->shutdown());
        const auto final = harness.backend->stats();
        assert(final.vm_coroutine_creations == final.vm_coroutine_releases);
        assert(final.prepared_ability_slots == 0U && final.prepared_event_slots == 0U);
        std::printf(
            "CREATION_OOM_PASS,permitted=%zu,registry_growth=%d,failures=%zu,provider_during_failure=0,stack_delta=0,"
            "slot_reused=1,roots=%zu,releases=%zu\n",
            permitted, registry_growth ? 1 : 0, allocation.failures,
            final.vm_coroutine_creations, final.vm_coroutine_releases
        );
        return 0;
    }

    int testCreationReentry(bool stop)
    {
        Harness harness{true, 1U, true, true};
        Provider provider;
        const auto binding = lux::script::bindScriptAbility<Ability>(provider);
        const std::array publications{publishScriptAbility(binding)};
        auto created = harness.create(publications);
        assert(created);
        auto system = std::move(*created);
        assert(system.prepare());
        g_observed_read_entries = 0U;
        assert(dispatchRuntimeHook(system, harness.sync_hook) == 1U);
        assert(g_observed_vm && g_observed_read_entries == 1U);
        auto* vm = g_observed_vm;
        const auto base = lua_gettop(vm);
        CreationAllocator allocation;
        allocation.original = lua_getallocf(vm, &allocation.context);
        allocation.permitted = (std::numeric_limits<std::size_t>::max)();
        struct Observation final { Harness& harness; ScriptSystem& system; bool stop; };
        Observation observation{harness, system, stop};
        allocation.observation_context = &observation;
        allocation.observe_growth = [](void* context) noexcept {
            auto& value = *static_cast<Observation*>(context);
            if (value.stop)
            {
                assert(value.system.requestStop());
                const auto busy = value.system.shutdown();
                assert(!busy && busy.error() == EScriptSystemError::ENDPOINT_BUSY);
            }
            else
                value.harness.registry.destroy(value.harness.entity);
            assert(value.harness.backend->stats().prepared_ability_slots == AbilityTraits::Description.methods.size());
        };
        lua_setallocf(vm, &CreationAllocator::allocate, &allocation);
        allocation.armed = true;
        assert(dispatchRuntimeHook(system, harness.async_hook) == 1U);
        allocation.armed = false;
        lua_setallocf(vm, allocation.original, allocation.context);
        assert(allocation.observe_growth == nullptr && allocation.failures == 0U && lua_gettop(vm) == base);
        if (stop)
        {
            assert(g_observed_read_entries == 2U && provider.reads == 2U && provider.writes == 2U);
            assert(provider.pending && system.activeContinuationCount() == 1U && system.failures().empty());
        }
        else
        {
            assert(g_observed_read_entries == 1U && provider.reads == 1U && provider.writes == 1U);
            assert(!provider.pending && system.activeContinuationCount() == 0U);
            assert(g_observed_steps == 1U && g_observed_step.state == EScriptStepState::FAILED);
            assert(g_observed_step.error.status == -1);
            // Existing invokeStep discards a returned step after native retirement. Its failure
            // log differs from invokeSync; observe the actual backend result before that boundary.
            assert(system.failures().empty());
        }
        assert(system.shutdown());
        const auto final = harness.backend->stats();
        assert(final.vm_coroutine_creations == final.vm_coroutine_releases);
        assert(final.prepared_ability_slots == 0U && final.prepared_event_slots == 0U);
        std::printf("CREATION_REENTRY_PASS,mode=%s,read_entries=%zu,reads=%zu,writes=%zu,roots=%zu,releases=%zu\n",
            stop ? "deferred-stop" : "native-retire", g_observed_read_entries, provider.reads, provider.writes,
            final.vm_coroutine_creations, final.vm_coroutine_releases);
        std::printf("CREATION_STEP_RESULT,observed=%zu,status=%d,runtime_errors=%zu\n",
            g_observed_steps, g_observed_step.error.status, system.failures().size());
        return 0;
    }
} // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::string_view{argv[1]} == "--creation-oom")
        return testCreationOom(static_cast<std::size_t>(std::strtoul(argv[2], nullptr, 10)));
    if (argc == 2 && std::string_view{argv[1]} == "--creation-registry-oom") return testCreationOom(2U, true);
    if (argc == 2 && std::string_view{argv[1]} == "--creation-retire") return testCreationReentry(false);
    if (argc == 2 && std::string_view{argv[1]} == "--creation-stop") return testCreationReentry(true);
    if (argc == 2 && std::string_view{argv[1]} == "--interpreter-only")
        g_execution_policy = lux::script::lua::ELuaExecutionPolicy::INTERPRETER_ONLY;
    Provider provider;
    static_assert(AbilityTraits::Description.name == "LuaRuntimeTest");
    static_assert(AbilityTraits::Description.display_name == "Lua Runtime Test");
    const auto binding = lux::script::bindScriptAbility<Ability>(provider);
    const std::array capabilities{publishScriptAbility(binding)};
    Harness harness;
    auto created = harness.create(capabilities);
    assert(created);
    auto system = std::move(*created);
    assert(system.prepare());
    assert(harness.backend->stats().vm_allocations.enabled);
    assert(harness.backend->stats().vm_allocations.allocations != 0U);
    assert(harness.backend->stats().vm_allocations.requested_bytes != 0U);

    assert(dispatchRuntimeHook(system, harness.sync_hook) == 1U);
    assert(provider.reads == 1U && provider.writes == 1U && provider.value == 12);
    assert(system.activeContinuationCount() == 0U);
    assert(system.activeAwaitableCount() == 0U);
    assert(harness.backend->stats().vm_coroutine_creations == 0U);
    assert(dispatchRuntimeHook(system, harness.scalar_hook) == 1U);
    assert(provider.bool_value);
    assert(provider.i32_value == (std::numeric_limits<std::int32_t>::max)());
    assert(provider.u32_value == (std::numeric_limits<std::uint32_t>::max)());
    assert(provider.f32_value == -12.5F);
    assert(provider.f64_value == 1234.125);

    provider.value = 7;
    assert(dispatchRuntimeHook(system, harness.async_hook) == 1U);
    assert(provider.pending.has_value());
    assert(provider.reads == 2U && provider.writes == 2U && provider.value == 12);
    assert(system.activeContinuationCount() == 1U);
    assert(system.activeAwaitableCount() == 1U);
    assert(dispatchRuntimeHook(system, harness.async_hook) == 1U);
    assert(provider.reads == 2U);

    provider.value = 100;
    assert(provider.pending->success(7));
    assert(provider.value == 100);
    assert(executeRuntimeStablePoint(system));
    assert(provider.value == 19);
    assert(provider.writes == 3U);
    assert(provider.pending->success(3));
    assert(executeRuntimeStablePoint(system));
    assert(provider.value == 22);
    assert(harness.backend->stats().vm_coroutine_creations == 1U);
    assert(harness.backend->stats().vm_coroutine_resumes == 2U);
    assert(harness.backend->stats().vm_coroutine_releases == 1U);
    assert(harness.backend->stats().vm_allocations.failures == 0U);
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
    assert(dispatchRuntimeHook(eager_system, eager.async_hook) == 1U);
    assert(eager_provider.value == 12);
    assert(eager_provider.eager_result.has_value() && *eager_provider.eager_result);
    assert(eager_system.activeContinuationCount() == 1U);
    assert(executeRuntimeStablePoint(eager_system));
    assert(eager_provider.value == 25);
    assert(eager_system.activeContinuationCount() == 1U);
    assert(executeRuntimeStablePoint(eager_system));
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
    assert(deliverRuntimeEvent(event_system, events.event_endpoint) == 2U);
    assert(event_provider.completion_count == 2U);
    assert(event_system.activeContinuationCount() == 2U);
    assert(event_provider.completions[0].success(10));
    assert(event_provider.completions[1].success(20));
    assert(executeRuntimeStablePoint(event_system));
    assert(event_provider.value == 32);
    assert(event_system.activeContinuationCount() == 0U);
    assert(event_system.shutdown());

    Provider waiter_provider;
    const auto waiter_binding = lux::script::bindScriptAbility<Ability>(waiter_provider);
    const std::array waiter_capabilities{publishScriptAbility(waiter_binding)};
    Harness waiter;
    auto waiter_created = waiter.create(waiter_capabilities);
    assert(waiter_created);
    auto waiter_system = std::move(*waiter_created);
    assert(waiter_system.prepare());
    assert(dispatchRuntimeHook(waiter_system, waiter.event_wait_hook) == 1U);
    assert(waiter_system.activeContinuationCount() == 1U);
    assert(waiter_system.stats().active_event_waiters == 1U);
    std::int32_t payload{41};
    {
        auto writer = waiter.async_event.begin(0U);
        assert(writer.record(payload));
    }
    payload = 99;
    assert(deliverRuntimeEvent(waiter_system, waiter.event_endpoint) == 1U);
    assert(waiter_provider.value == 7);
    assert(waiter_system.stats().active_event_waiters == 0U);
    assert(waiter_system.activeContinuationCount() == 2U);
    assert(executeRuntimeStablePoint(waiter_system));
    assert(waiter_provider.value == 41);
    assert(waiter_system.activeContinuationCount() == 1U);
    assert(waiter_system.shutdown());

    Provider targeted_provider;
    const auto targeted_binding = lux::script::bindScriptAbility<Ability>(targeted_provider);
    const std::array targeted_capabilities{publishScriptAbility(targeted_binding)};
    Harness targeted;
    auto targeted_created = targeted.create(targeted_capabilities);
    assert(targeted_created);
    auto targeted_system = std::move(*targeted_created);
    assert(targeted_system.prepare());
    assert(dispatchRuntimeHook(targeted_system, targeted.target_wait_hook) == 1U);
    assert(targeted_system.stats().active_event_waiters == 1U);
    const auto other = targeted.registry.create();
    {
        auto writer = targeted.target_event.begin(0U);
        assert(writer.record(other, 71));
    }
    assert(deliverRuntimeEvent(targeted_system, targeted.target_event_endpoint) == 1U);
    assert(targeted_system.stats().active_event_waiters == 1U);
    assert(executeRuntimeStablePoint(targeted_system));
    assert(targeted_provider.value == 7);
    {
        auto writer = targeted.target_event.begin(0U);
        assert(writer.record(targeted.entity, 88));
    }
    assert(deliverRuntimeEvent(targeted_system, targeted.target_event_endpoint) == 1U);
    assert(targeted_provider.value == 7);
    assert(executeRuntimeStablePoint(targeted_system));
    assert(targeted_provider.value == 88);
    assert(targeted_system.stats().active_event_waiters == 0U);
    assert(targeted_system.shutdown());

    Provider event_retirement_provider;
    const auto event_retirement_binding = lux::script::bindScriptAbility<Ability>(event_retirement_provider);
    const std::array event_retirement_capabilities{publishScriptAbility(event_retirement_binding)};
    Harness event_retirement;
    auto event_retirement_created = event_retirement.create(event_retirement_capabilities);
    assert(event_retirement_created);
    auto event_retirement_system = std::move(*event_retirement_created);
    assert(event_retirement_system.prepare());
    assert(dispatchRuntimeHook(event_retirement_system, event_retirement.event_wait_hook) == 1U);
    assert(event_retirement_system.stats().active_event_waiters == 1U);
    event_retirement.registry.destroy(event_retirement.entity);
    const auto event_retired = executeRuntimeStablePoint(event_retirement_system);
    assert(event_retired);
    assert(event_retirement_system.stats().active_event_waiters == 0U);
    const auto writes_before_late_event = event_retirement_provider.writes;
    {
        auto writer = event_retirement.async_event.begin(0U);
        assert(writer.record(109));
    }
    assert(deliverRuntimeEvent(event_retirement_system, event_retirement.event_endpoint) == 1U);
    static_cast<void>(executeRuntimeStablePoint(event_retirement_system));
    assert(event_retirement_provider.writes == writes_before_late_event);
    assert(event_retirement_system.shutdown());

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
    assert(deliverRuntimeEvent(limited_system, limited.event_endpoint) == 2U);
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
    assert(dispatchRuntimeHook(retiring_system, retiring.async_hook) == 1U);
    assert(retiring_provider.pending.has_value());
    const auto late_completion = *retiring_provider.pending;
    retiring.registry.destroy(retiring.entity);
    const auto retired = executeRuntimeStablePoint(retiring_system);
    assert(retired);
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
    assert(dispatchRuntimeHook(failed_system, failed.async_hook) == 1U);
    assert(failed_provider.pending.has_value());
    assert(failed_provider.pending->fail({91}));
    const auto failed_stable = executeRuntimeStablePoint(failed_system);
    assert(failed_stable && failed_stable->first_instance_error == EScriptSystemError::INVOCATION_FAILURE);
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
    assert(dispatchRuntimeHook(rejected_system, rejected.async_hook) == 1U);
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
    assert(dispatchRuntimeHook(raw_yield_system, raw_yield.async_hook) == 1U);
    assert(!raw_yield_system.failures().empty());
    assert(raw_yield_system.activeContinuationCount() == 0U);
    assert(raw_yield_system.activeAwaitableCount() == 0U);
    assert(raw_yield_system.shutdown());

    Harness undeclared{false};
    auto undeclared_created = undeclared.create({});
    assert(undeclared_created);
    auto undeclared_system = std::move(*undeclared_created);
    assert(undeclared_system.prepare());
    assert(dispatchRuntimeHook(undeclared_system, undeclared.sync_hook) == 1U);
    assert(!undeclared_system.failures().empty());
    assert(undeclared_system.failures().front().error == EScriptSystemError::INVOCATION_FAILURE);
    assert(undeclared_system.shutdown());
    return 0;
}

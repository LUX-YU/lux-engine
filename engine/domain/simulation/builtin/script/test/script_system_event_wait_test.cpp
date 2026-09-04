#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/scripting/ScriptEventSource.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    inline constexpr lux::system::SystemInstanceId kSystem{0x5500U};
    inline constexpr HookPointId kDispatchHook{0x5501U};
    inline constexpr EventPointId kBroadcastStart{0x5502U};
    inline constexpr EventPointId kBroadcastWait{0x5503U};
    inline constexpr EventPointId kTargetedStart{0x5504U};
    inline constexpr EventPointId kTargetedWait{0x5505U};
    inline constexpr EventPointId kBroadcastStartSecond{0x5506U};
    inline constexpr EventPointId kBroadcastFaultSecond{0x5507U};
    inline constexpr lux::script::ScriptSymbolId kStartSymbol{0x5510U};
    inline constexpr lux::script::ScriptSymbolId kCallbackSymbol{0x5511U};

    enum class ERequirementMutation : std::uint8_t
    {
        NONE,
        EVENT_ID,
        SYSTEM_ID,
        ROUTE,
        PAYLOAD_LAYOUT,
        SCHEMA_HASH,
        SCHEMA_VERSION,
    };

    [[nodiscard]] lux::asset::AssetId assetId()
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[0] = 0x55U;
        return lux::asset::AssetId{bytes};
    }

    [[nodiscard]] lux::world::WorldObjectId objectId()
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[0] = 0x56U;
        return lux::world::WorldObjectId{uuids::uuid{bytes}};
    }

    [[nodiscard]] SimulationDescription makeSimulation()
    {
        constexpr std::array hooks{makeHookPointSpec<void()>(kDispatchHook, "dispatch")};
        constexpr std::array events{
            makeEventPointSpec<std::int32_t>(
                kBroadcastStart,
                "broadcast-start",
                kDispatchHook,
                EEventRoute::SIMULATION_BROADCAST,
                "lux.i32",
                1U
            ),
            makeEventPointSpec<std::int32_t>(
                kBroadcastWait,
                "broadcast-wait",
                kDispatchHook,
                EEventRoute::SIMULATION_BROADCAST,
                "lux.i32",
                1U
            ),
            makeEventPointSpec<std::int32_t>(
                kTargetedStart,
                "targeted-start",
                kDispatchHook,
                EEventRoute::ENTITY_TARGETED,
                "lux.i32",
                1U
            ),
            makeEventPointSpec<std::int32_t>(
                kTargetedWait,
                "targeted-wait",
                kDispatchHook,
                EEventRoute::ENTITY_TARGETED,
                "lux.i32",
                1U
            ),
            makeEventPointSpec<std::int32_t>(
                kBroadcastStartSecond,
                "broadcast-start-second",
                kDispatchHook,
                EEventRoute::SIMULATION_BROADCAST,
                "lux.i32",
                1U
            ),
            makeEventPointSpec<std::int32_t>(
                kBroadcastFaultSecond,
                "broadcast-fault-second",
                kDispatchHook,
                EEventRoute::SIMULATION_BROADCAST,
                "lux.i32",
                1U
            )
        };
        const SimulationSystemDescription system{
            .type = {.canonical_name = "lux.test.event-wait", .version = 1U},
            .hooks = hooks,
            .events = events
        };
        SimulationDescriptionBuilder builder;
        assert(builder.addSystem(kSystem, "event-wait", system));
        auto result = std::move(builder).build();
        assert(result);
        return std::move(*result);
    }

    [[nodiscard]] lux::script::ScriptArtifact makeArtifact(
        EEventRoute wait_route,
        ERequirementMutation mutation
    )
    {
        const auto payload = lux::rdesc::makeScriptValueType<std::int32_t>(lux::semantic::EValuePass::CONST_REF);
        lux::rdesc::Script description;
        description.module_name = "lux.test.event-wait.script";
        description.exports = {
            {"start", kStartSymbol, {payload}, {}},
            {"callback", kCallbackSymbol, {payload}, {}}
        };
        const bool targeted = wait_route == EEventRoute::ENTITY_TARGETED;
        lux::script::ScriptEventSourceDescription requirement{
            "EventWait",
            targeted ? "targeted" : "broadcast",
            kSystem.value,
            targeted ? kTargetedWait.value : kBroadcastWait.value,
            targeted ? lux::script::EScriptEventRoute::ENTITY_TARGETED
                     : lux::script::EScriptEventRoute::SIMULATION_BROADCAST,
            {"lux.i32", lux::semantic::typeId("lux.i32"), LUX_SCRIPT_VK_INT32, 4U, 4U},
            lux::semantic::typeId("lux.i32"),
            1U
        };
        switch (mutation)
        {
        case ERequirementMutation::EVENT_ID: requirement.event_id += 0x1000U; break;
        case ERequirementMutation::SYSTEM_ID: requirement.system_id += 0x1000U; break;
        case ERequirementMutation::ROUTE:
            requirement.route = targeted ? lux::script::EScriptEventRoute::SIMULATION_BROADCAST
                                         : lux::script::EScriptEventRoute::ENTITY_TARGETED;
            break;
        case ERequirementMutation::PAYLOAD_LAYOUT: requirement.payload.size *= 2U; break;
        case ERequirementMutation::SCHEMA_HASH: ++requirement.payload_schema_hash; break;
        case ERequirementMutation::SCHEMA_VERSION: ++requirement.payload_schema_version; break;
        case ERequirementMutation::NONE: break;
        }
        description.event_requirements.push_back(std::move(requirement));
        description.body = lux::rdesc::CppStaticScript{"event-wait-fixture"};
        auto result = lux::script::ScriptArtifact::create(std::move(description), {});
        assert(result);
        return std::move(*result);
    }

    enum class ECallbackAction : std::uint8_t
    {
        NONE,
        WAIT_ONCE,
        DESTROY_SELF,
        FAIL,
        NESTED_DISPATCH,
    };

    struct BackendState;

    struct BackendInstance final
    {
        BackendState* owner{};
        ecs::Entity self{ecs::NullEntity};
    };

    struct PreparedCall final
    {
        BackendInstance* instance{};
        lux::script::ScriptSymbolId symbol{};
    };

    struct Continuation final
    {
        BackendState* owner{};
    };

    struct BackendState final
    {
        ScriptEventWaitRequest wait_request;
        ECallbackAction callback_action{ECallbackAction::NONE};
        ecs::Registry* registry{};
        std::size_t callback_calls{};
        std::size_t step_calls{};
        std::size_t instance_creates{};
        std::size_t resumes{};
        std::size_t continuation_destroys{};
        std::size_t wait_once_count{};
        std::size_t nested_dispatch_count{};
        std::optional<EScriptEventWaitError> wait_error;
        std::vector<std::int32_t> resume_values;
        void* nested_context{};
        void (*nested_dispatch)(void*, std::int32_t) noexcept{};
    };

    void destroyContinuation(void* opaque) noexcept
    {
        auto* continuation = static_cast<Continuation*>(opaque);
        ++continuation->owner->continuation_destroys;
        delete continuation;
    }

    ScriptStepResult resumeContinuation(
        void* opaque,
        ScriptStepContext&,
        const ScriptResumePacket& packet
    ) noexcept
    {
        auto& continuation = *static_cast<Continuation*>(opaque);
        auto& state = *continuation.owner;
        assert(packet.state == EScriptAwaitableState::READY);
        assert(packet.value != nullptr && packet.value->type.valid());
        assert(packet.value->type.type_id == lux::semantic::typeId("lux.i32"));
        assert(packet.value->bytes.size() == sizeof(std::int32_t));
        std::int32_t value{};
        std::memcpy(std::addressof(value), packet.value->bytes.data(), sizeof(value));
        state.resume_values.push_back(value);
        ++state.resumes;
        return ScriptStepResult::completed();
    }

    [[nodiscard]] ScriptStepResult beginWait(
        BackendState& state,
        ScriptStepContext& context,
        ScriptBackendContinuation& output
    ) noexcept
    {
        const auto waiting = context.event_waits.wait(state.wait_request);
        if (!waiting)
        {
            state.wait_error = waiting.error();
            return ScriptStepResult::failed(1000 + static_cast<std::int32_t>(waiting.error()));
        }
        auto* continuation = new (std::nothrow) Continuation{std::addressof(state)};
        if (continuation == nullptr)
        {
            context.awaitables.discard(*waiting);
            return ScriptStepResult::failed(1100);
        }
        output = {continuation, &resumeContinuation, &destroyContinuation};
        return ScriptStepResult::suspended(*waiting);
    }

    ScriptStepResult invokeStep(
        void* opaque,
        lux_script_call_frame& frame,
        ScriptStepContext& context,
        ScriptBackendContinuation& output
    ) noexcept
    {
        auto& call = *static_cast<PreparedCall*>(opaque);
        auto& state = *call.instance->owner;
        assert(frame.arg_count == 1U && frame.args != nullptr);
        ++state.step_calls;
        if (call.symbol == kStartSymbol)
            return beginWait(state, context, output);
        if (call.symbol != kCallbackSymbol)
            return ScriptStepResult::failed(1200);

        ++state.callback_calls;
        if (state.callback_action == ECallbackAction::FAIL)
            return ScriptStepResult::failed(1300);
        if (state.callback_action == ECallbackAction::DESTROY_SELF)
        {
            assert(state.registry != nullptr && call.instance->self != ecs::NullEntity);
            state.registry->destroy(call.instance->self);
            return ScriptStepResult::completed();
        }
        if (state.callback_action == ECallbackAction::NESTED_DISPATCH && state.nested_dispatch_count++ == 0U)
        {
            assert(state.nested_dispatch != nullptr);
            state.nested_dispatch(state.nested_context, 71);
            state.nested_dispatch(state.nested_context, 72);
        }
        if (state.callback_action == ECallbackAction::WAIT_ONCE && state.wait_once_count++ == 0U)
            return beginWait(state, context, output);
        return ScriptStepResult::completed();
    }

    int invokeSync(lux_script_call_frame*)
    {
        return 0;
    }

    EScriptBackendResult createInstance(
        void* context,
        const ScriptInstanceCreateContext& create,
        const lux::script::ScriptArtifact&,
        ScriptBackendInstance& output
    ) noexcept
    {
        auto* instance = new (std::nothrow) BackendInstance();
        if (instance == nullptr)
            return EScriptBackendResult::ALLOCATION_FAILURE;
        instance->owner = static_cast<BackendState*>(context);
        ++instance->owner->instance_creates;
        if (const auto* entity = std::get_if<EntityScriptScope>(&create.scope))
            instance->self = entity->self;
        output.value = instance;
        return EScriptBackendResult::SUCCESS;
    }

    EScriptBackendResult prepareMethod(
        void*,
        ScriptBackendInstance instance,
        const lux::rdesc::ScriptFunction& function,
        ScriptBackendPreparedMethod& output
    ) noexcept
    {
        auto* prepared = new (std::nothrow) PreparedCall{
            static_cast<BackendInstance*>(instance.value),
            function.symbol_id
        };
        if (prepared == nullptr)
            return EScriptBackendResult::ALLOCATION_FAILURE;
        output = {
            prepared,
            lux::script::BoundScriptCall{&invokeSync, instance.value},
            BoundScriptStepCall{prepared, &invokeStep}
        };
        return EScriptBackendResult::SUCCESS;
    }

    void releaseMethod(void*, ScriptBackendInstance, ScriptBackendPreparedMethod method) noexcept
    {
        delete static_cast<PreparedCall*>(method.token);
    }

    void destroyInstance(void*, ScriptBackendInstance instance) noexcept
    {
        delete static_cast<BackendInstance*>(instance.value);
    }

    struct HarnessOptions final
    {
        bool entity_scope{};
        bool bind_start{true};
        bool bind_callback{};
        EEventRoute wait_route{EEventRoute::SIMULATION_BROADCAST};
        ECallbackAction callback_action{ECallbackAction::NONE};
        bool ownership_pair{};
        bool immediate_wait_endpoint{};
        bool disable_wait_copy{};
        bool fail_wait_copy{};
        ERequirementMutation requirement_mutation{ERequirementMutation::NONE};
        std::optional<EScriptSystemError> expected_prepare_error;
        std::size_t occurrence_capacity{8U};
        ScriptRuntimeLimits limits{32U, 1U, 16U, 16U, 16U, 16U, 64U, 16U, 16U, 16U, 16U, 16U};
    };

    struct Harness final
    {
        struct ImmediateBroadcastEndpoint final
        {
            void* lane_context{};
            ScriptEventLane lane{};

            [[nodiscard]] ScriptEventEndpointDescriptor descriptor() noexcept
            {
                using Traits = lux::semantic::TypeTraits<std::int32_t>;
                return {
                    kSystem,
                    kBroadcastWait,
                    EEventRoute::SIMULATION_BROADCAST,
                    lux::semantic::makeType<std::int32_t>(lux::semantic::EValuePass::CONST_REF),
                    {{lux::semantic::typeId(Traits::CanonicalName),
                      Traits::CanonicalName,
                      Traits::AbiKind,
                      Traits::Size,
                      Traits::Alignment},
                     &copyPayload},
                    this,
                    &connect,
                    &disconnect
                };
            }

            void emit(std::int32_t payload) noexcept
            {
                auto slot = lux::simulation::script::detail::argumentSlot(payload);
                lux_script_call_frame frame{&slot, 1U, 0U, nullptr, 0U, 0U, nullptr, nullptr};
                lane(lane_context, ecs::NullEntity, frame);
            }

            static void emitErased(void* context, std::int32_t payload) noexcept
            {
                static_cast<ImmediateBroadcastEndpoint*>(context)->emit(payload);
            }

            static EndpointConnectResult connect(
                void* context,
                void* lane_context,
                ScriptEventLane lane
            ) noexcept
            {
                auto& self = *static_cast<ImmediateBroadcastEndpoint*>(context);
                self.lane_context = lane_context;
                self.lane = lane;
                return {{1U, 1U}, EEndpointMutationError::NONE};
            }

            static EEndpointMutationError disconnect(void* context, EndpointConnectionToken token) noexcept
            {
                auto& self = *static_cast<ImmediateBroadcastEndpoint*>(context);
                if (!token.valid() || self.lane == nullptr)
                    return EEndpointMutationError::INVALID_TOKEN;
                self.lane_context = nullptr;
                self.lane = nullptr;
                return EEndpointMutationError::NONE;
            }

            static bool copyPayload(
                void*,
                const lux_script_value_slot& input,
                std::span<std::byte> output
            ) noexcept
            {
                if (input.data == nullptr || input.size != sizeof(std::int32_t) ||
                    output.size() != sizeof(std::int32_t))
                {
                    return false;
                }
                std::memcpy(output.data(), input.data, output.size());
                return true;
            }
        };

        static bool rejectPayload(
            void*,
            const lux_script_value_slot&,
            std::span<std::byte>
        ) noexcept
        {
            return false;
        }

        explicit Harness(HarnessOptions options)
            : simulation(makeSimulation()),
              artifact(makeArtifact(options.wait_route, options.requirement_mutation)),
              asset(assetId()),
              object(objectId())
        {
            if (options.entity_scope)
                entity = registry.create();

            ScriptSystemDescriptionBuilder description_builder;
            ScriptMountDescription mount{
                ScriptMountId{1U},
                asset,
                options.entity_scope ? ScriptMountScope{EntityScriptMount{object}}
                                     : ScriptMountScope{SimulationScriptMount{}},
                true,
                {}
            };
            if (options.bind_start)
            {
                const auto event = options.entity_scope ? kTargetedStart : kBroadcastStart;
                mount.bindings.push_back({kStartSymbol, EventScriptTarget{kSystem, event}});
            }
            if (options.bind_callback)
            {
                const auto event = options.wait_route == EEventRoute::ENTITY_TARGETED
                    ? kTargetedWait
                    : kBroadcastWait;
                mount.bindings.push_back({kCallbackSymbol, EventScriptTarget{kSystem, event}});
            }
            assert(description_builder.addMount(std::move(mount)));
            if (options.ownership_pair)
            {
                assert(!options.entity_scope);
                assert(description_builder.addMount({
                    ScriptMountId{2U},
                    asset,
                    SimulationScriptMount{},
                    true,
                    {
                        {kStartSymbol, EventScriptTarget{kSystem, kBroadcastStartSecond}},
                        {kCallbackSymbol, EventScriptTarget{kSystem, kBroadcastFaultSecond}}
                    }
                }));
            }
            auto built = std::move(description_builder).build(simulation);
            assert(built);
            description = std::move(*built);

            assert(broadcast_start.prepare(1U, options.occurrence_capacity, 1U) == EEndpointMutationError::NONE);
            assert(broadcast_wait.prepare(1U, options.occurrence_capacity, 1U) == EEndpointMutationError::NONE);
            assert(targeted_start.prepare(1U, options.occurrence_capacity, 1U) == EEndpointMutationError::NONE);
            assert(targeted_wait.prepare(1U, options.occurrence_capacity, 1U) == EEndpointMutationError::NONE);
            assert(broadcast_start_second.prepare(1U, options.occurrence_capacity, 1U) == EEndpointMutationError::NONE);
            assert(broadcast_fault_second.prepare(1U, options.occurrence_capacity, 1U) == EEndpointMutationError::NONE);

            broadcast_start_bridge = std::make_unique<BroadcastBridge>(kSystem, kBroadcastStart, broadcast_start);
            broadcast_wait_bridge = std::make_unique<BroadcastBridge>(kSystem, kBroadcastWait, broadcast_wait);
            targeted_start_bridge = std::make_unique<TargetedBridge>(kSystem, kTargetedStart, targeted_start);
            targeted_wait_bridge = std::make_unique<TargetedBridge>(kSystem, kTargetedWait, targeted_wait);
            broadcast_start_second_bridge =
                std::make_unique<BroadcastBridge>(kSystem, kBroadcastStartSecond, broadcast_start_second);
            broadcast_fault_second_bridge =
                std::make_unique<BroadcastBridge>(kSystem, kBroadcastFaultSecond, broadcast_fault_second);
            endpoints = {
                broadcast_start_bridge->descriptor(),
                options.immediate_wait_endpoint ? immediate_wait.descriptor() : broadcast_wait_bridge->descriptor(),
                targeted_start_bridge->descriptor(),
                targeted_wait_bridge->descriptor(),
                broadcast_start_second_bridge->descriptor(),
                broadcast_fault_second_bridge->descriptor()
            };
            const auto wait_endpoint = options.wait_route == EEventRoute::ENTITY_TARGETED ? 3U : 1U;
            const auto wait_event = options.wait_route == EEventRoute::ENTITY_TARGETED
                ? kTargetedWait
                : kBroadcastWait;
            const auto projected = projectScriptEventSource(
                simulation.findEvent(kSystem, wait_event),
                endpoints[wait_endpoint],
                "EventWait",
                options.wait_route == EEventRoute::ENTITY_TARGETED ? "targeted" : "broadcast"
            );
            assert(projected);
            if (options.requirement_mutation == ERequirementMutation::NONE)
                assert(*projected == artifact.description().event_requirements.front());
            if (options.disable_wait_copy)
            {
                endpoints[wait_endpoint].payload_projection.copy = nullptr;
            }
            if (options.fail_wait_copy)
                endpoints[wait_endpoint].payload_projection.copy = &rejectPayload;

            backend_state.wait_request = {
                kSystem,
                options.wait_route == EEventRoute::ENTITY_TARGETED ? kTargetedWait : kBroadcastWait,
                options.wait_route
            };
            backend_state.callback_action = options.callback_action;
            backend_state.registry = std::addressof(registry);
            if (options.immediate_wait_endpoint)
            {
                backend_state.nested_context = std::addressof(immediate_wait);
                backend_state.nested_dispatch = &ImmediateBroadcastEndpoint::emitErased;
            }
            backend = {
                lux::rdesc::Script::Kind::CPP_STATIC,
                std::addressof(backend_state),
                &createInstance,
                &prepareMethod,
                &releaseMethod,
                &destroyInstance
            };

            auto created = ScriptSystem::create(
                simulation,
                description,
                registry,
                clock,
                options.limits,
                {this, &resolveArtifact},
                options.entity_scope ? WorldObjectResolver{this, &resolveWorld} : WorldObjectResolver{},
                {},
                std::span{std::addressof(backend), 1U},
                {},
                endpoints
            );
            if (!created)
            {
                std::fprintf(
                    stderr,
                    "Event waiter harness create failed: %u; limits=%zu/%zu/%zu\n",
                    static_cast<unsigned>(created.error()),
                    options.limits.awaitable_capacity,
                    options.limits.event_wait_capacity,
                    options.limits.external_completion_capacity
                );
            }
            assert(created);
            system = std::make_unique<ScriptSystem>(std::move(*created));
            const auto prepared = system->prepare();
            if (options.expected_prepare_error)
            {
                if (prepared || prepared.error() != *options.expected_prepare_error)
                {
                    std::fprintf(
                        stderr,
                        "Expected prepare error %u for mutation %u, got %u\n",
                        static_cast<unsigned>(*options.expected_prepare_error),
                        static_cast<unsigned>(options.requirement_mutation),
                        prepared ? 255U : static_cast<unsigned>(prepared.error())
                    );
                }
                assert(!prepared && prepared.error() == *options.expected_prepare_error);
                return;
            }
            if (!prepared)
            {
                std::fprintf(
                    stderr,
                    "Event waiter harness prepare failed: %u\n",
                    static_cast<unsigned>(prepared.error())
                );
            }
            assert(prepared);
        }

        ~Harness()
        {
            if (system)
                assert(system->shutdown());
        }

        static bool resolveArtifact(
            void* context,
            const lux::asset::AssetId& requested,
            ResolvedScriptArtifact& output
        ) noexcept
        {
            auto& self = *static_cast<Harness*>(context);
            if (requested != self.asset)
                return false;
            output.artifact = std::addressof(self.artifact);
            return true;
        }

        static bool resolveWorld(
            void* context,
            const lux::world::WorldObjectId& requested,
            ecs::Entity& output
        ) noexcept
        {
            auto& self = *static_cast<Harness*>(context);
            if (requested != self.object || self.entity == ecs::NullEntity)
                return false;
            output = self.entity;
            return true;
        }

        void recordBroadcastStart(std::int32_t payload)
        {
            auto writer = broadcast_start.begin(0U);
            assert(writer.record(payload));
        }

        void recordBroadcastWait(std::int32_t payload)
        {
            auto writer = broadcast_wait.begin(0U);
            assert(writer.record(payload));
        }

        void recordBroadcastStartSecond(std::int32_t payload)
        {
            auto writer = broadcast_start_second.begin(0U);
            assert(writer.record(payload));
        }

        void recordBroadcastFaultSecond(std::int32_t payload)
        {
            auto writer = broadcast_fault_second.begin(0U);
            assert(writer.record(payload));
        }

        void emitImmediateWait(std::int32_t payload) noexcept
        {
            immediate_wait.emit(payload);
        }

        void recordTargetedStart(ecs::Entity target, std::int32_t payload)
        {
            auto writer = targeted_start.begin(0U);
            assert(writer.record(target, payload));
        }

        void recordTargetedWait(ecs::Entity target, std::int32_t payload)
        {
            auto writer = targeted_wait.begin(0U);
            assert(writer.record(target, payload));
        }

        using BroadcastPoint = EventPoint<SimulationBroadcastRoute, std::int32_t>;
        using TargetedPoint = EventPoint<EntityTargetedRoute<ecs::Entity>, std::int32_t>;
        using BroadcastBridge = ScriptEventEndpoint<SimulationBroadcastRoute, std::int32_t>;
        using TargetedBridge = ScriptEventEndpoint<EntityTargetedRoute<ecs::Entity>, std::int32_t>;

        SimulationDescription simulation;
        lux::script::ScriptArtifact artifact;
        lux::asset::AssetId asset;
        lux::world::WorldObjectId object;
        ecs::Registry registry;
        ecs::Entity entity{ecs::NullEntity};
        SimulationClock clock;
        ScriptSystemDescription description;
        BroadcastPoint broadcast_start;
        BroadcastPoint broadcast_wait;
        TargetedPoint targeted_start;
        TargetedPoint targeted_wait;
        BroadcastPoint broadcast_start_second;
        BroadcastPoint broadcast_fault_second;
        ImmediateBroadcastEndpoint immediate_wait;
        std::unique_ptr<BroadcastBridge> broadcast_start_bridge;
        std::unique_ptr<BroadcastBridge> broadcast_wait_bridge;
        std::unique_ptr<TargetedBridge> targeted_start_bridge;
        std::unique_ptr<TargetedBridge> targeted_wait_bridge;
        std::unique_ptr<BroadcastBridge> broadcast_start_second_bridge;
        std::unique_ptr<BroadcastBridge> broadcast_fault_second_bridge;
        std::array<ScriptEventEndpointDescriptor, 6U> endpoints;
        BackendState backend_state;
        ScriptBackendDescriptor backend;
        std::unique_ptr<ScriptSystem> system;
    };

    void testBroadcastSemantics()
    {
        {
            Harness callback_only{{.bind_start = false, .bind_callback = true}};
            callback_only.recordBroadcastWait(1);
            assert(callback_only.broadcast_wait.drain() == 1U);
            assert(callback_only.backend_state.callback_calls == 1U);
            assert(callback_only.system->stats().active_event_waiters == 0U);
        }

        Harness harness{{.bind_callback = true}};
        harness.recordBroadcastStart(1);
        harness.recordBroadcastStart(2);
        assert(harness.broadcast_start.drain() == 2U);
        auto waiting = harness.system->stats();
        assert(waiting.active_event_waiters == 2U);
        assert(waiting.event_waiter_high_water == 2U);
        assert(waiting.active_awaitables == 2U);
        assert(waiting.active_continuations == 2U);

        std::int32_t payload{42};
        harness.recordBroadcastWait(payload);
        payload = 99;
        assert(harness.broadcast_wait.drain() == 1U);
        assert(harness.system->stats().event_waiter_dispatch_visits -
            waiting.event_waiter_dispatch_visits == 2U);
        assert(harness.backend_state.callback_calls == 1U);
        assert(harness.backend_state.resumes == 0U);
        assert(harness.system->stats().active_event_waiters == 0U);
        assert(harness.system->executeStablePoint());
        assert(harness.backend_state.resumes == 2U);
        assert(harness.backend_state.resume_values == std::vector<std::int32_t>({42, 42}));

        harness.recordBroadcastWait(7);
        assert(harness.broadcast_wait.drain() == 1U);
        assert(harness.system->executeStablePoint());
        assert(harness.backend_state.resumes == 2U);
    }

    void testRegistrationCutoff()
    {
        Harness harness{{
            .bind_callback = true,
            .callback_action = ECallbackAction::WAIT_ONCE
        }};
        harness.recordBroadcastStart(1);
        assert(harness.broadcast_start.drain() == 1U);
        harness.recordBroadcastWait(10);
        assert(harness.broadcast_wait.drain() == 1U);
        assert(harness.backend_state.callback_calls == 1U);
        assert(harness.system->stats().active_event_waiters == 1U);
        assert(harness.system->executeStablePoint());
        assert(harness.backend_state.resume_values == std::vector<std::int32_t>({10}));

        harness.recordBroadcastWait(20);
        assert(harness.broadcast_wait.drain() == 1U);
        assert(harness.system->stats().active_event_waiters == 0U);
        assert(harness.system->executeStablePoint());
        assert(harness.backend_state.resume_values == std::vector<std::int32_t>({10, 20}));
    }

    void testTargetedAndRetirement()
    {
        Harness harness{{
            .entity_scope = true,
            .bind_callback = true,
            .wait_route = EEventRoute::ENTITY_TARGETED
        }};
        const auto old_entity = harness.entity;
        const auto other = harness.registry.create();
        harness.recordTargetedStart(old_entity, 1);
        assert(harness.targeted_start.drain() == 1U);
        assert(harness.system->stats().active_event_waiters == 1U);

        harness.recordTargetedWait(other, 2);
        const auto visits_before_wrong_target = harness.system->stats().event_waiter_dispatch_visits;
        assert(harness.targeted_wait.drain() == 1U);
        assert(harness.system->stats().active_event_waiters == 1U);
        assert(harness.system->stats().event_waiter_dispatch_visits == visits_before_wrong_target);
        assert(harness.backend_state.callback_calls == 0U);

        harness.recordTargetedWait(old_entity, 3);
        assert(harness.targeted_wait.drain() == 1U);
        assert(harness.backend_state.callback_calls == 1U);
        assert(harness.backend_state.resumes == 0U);
        assert(harness.system->executeStablePoint());
        assert(harness.backend_state.resume_values == std::vector<std::int32_t>({3}));

        harness.backend_state.callback_action = ECallbackAction::DESTROY_SELF;
        harness.recordTargetedStart(old_entity, 4);
        assert(harness.targeted_start.drain() == 1U);
        harness.recordTargetedWait(old_entity, 5);
        assert(harness.targeted_wait.drain() == 1U);
        assert(harness.backend_state.resumes == 1U);
        assert(harness.system->stats().active_event_waiters == 0U);
        const auto retired = harness.system->executeStablePoint();
        assert(!retired && retired.error() == EScriptSystemError::WORLD_OBJECT_NOT_RESOLVED);
        assert(harness.system->activeInstanceCount() == 0U);

        harness.recordTargetedWait(old_entity, 6);
        assert(harness.targeted_wait.drain() == 1U);
        assert(harness.system->executeStablePoint().error() == EScriptSystemError::WORLD_OBJECT_NOT_RESOLVED);
        assert(harness.backend_state.resumes == 1U);

        harness.entity = harness.registry.create();
        assert(harness.system->executeStablePoint());
        harness.backend_state.callback_action = ECallbackAction::NONE;
        harness.recordTargetedStart(harness.entity, 7);
        assert(harness.targeted_start.drain() == 1U);
        harness.recordTargetedWait(old_entity, 8);
        assert(harness.targeted_wait.drain() == 1U);
        assert(harness.system->stats().active_event_waiters == 1U);
        harness.recordTargetedWait(harness.entity, 9);
        assert(harness.targeted_wait.drain() == 1U);
        assert(harness.system->executeStablePoint());
        assert(harness.backend_state.resume_values == std::vector<std::int32_t>({3, 9}));
    }

    void testTargetedScopeRejection()
    {
        Harness harness{{.wait_route = EEventRoute::ENTITY_TARGETED}};
        harness.recordBroadcastStart(1);
        assert(harness.broadcast_start.drain() == 1U);
        assert(harness.backend_state.wait_error == EScriptEventWaitError::SCOPE_MISMATCH);
        assert(harness.system->stats().active_event_waiters == 0U);
        assert(harness.system->executeStablePoint());
        assert(harness.system->activeInstanceCount() == 0U);
    }

    void testCapacityFailure()
    {
        HarnessOptions options;
        options.occurrence_capacity = 2U;
        options.limits = {8U, 1U, 2U, 2U, 2U, 1U, 64U, 1U, 2U, 2U, 1U, 2U};
        Harness harness{options};
        harness.recordBroadcastStart(1);
        harness.recordBroadcastStart(2);
        assert(harness.broadcast_start.drain() == 2U);
        assert(harness.backend_state.wait_error == EScriptEventWaitError::WAITER_CAPACITY_EXCEEDED);
        assert(harness.system->stats().active_event_waiters == 1U);
        assert(harness.system->executeStablePoint());
        assert(harness.system->activeInstanceCount() == 0U);
        const auto stats = harness.system->stats();
        assert(stats.active_event_waiters == 0U);
        assert(stats.active_awaitables == 0U);
        assert(stats.active_continuations == 0U);
    }

    void testAwaitableCapacityFailure()
    {
        HarnessOptions options;
        options.occurrence_capacity = 2U;
        options.limits = {8U, 1U, 2U, 2U, 1U, 1U, 64U, 1U, 2U, 2U, 2U, 2U};
        Harness harness{options};
        harness.recordBroadcastStart(1);
        harness.recordBroadcastStart(2);
        assert(harness.broadcast_start.drain() == 2U);
        assert(harness.backend_state.wait_error == EScriptEventWaitError::AWAITABLE_CAPACITY_EXCEEDED);
        assert(harness.system->executeStablePoint());
        const auto stats = harness.system->stats();
        assert(stats.active_event_waiters == 0U);
        assert(stats.active_awaitables == 0U);
        assert(stats.active_continuations == 0U);
    }

    void testResumeQueueFailure()
    {
        HarnessOptions options;
        options.occurrence_capacity = 2U;
        options.limits = {8U, 1U, 2U, 2U, 2U, 1U, 64U, 1U, 2U, 2U, 2U, 2U};
        Harness harness{options};
        harness.recordBroadcastStart(1);
        harness.recordBroadcastStart(2);
        assert(harness.broadcast_start.drain() == 2U);
        harness.recordBroadcastWait(12);
        assert(harness.broadcast_wait.drain() == 1U);
        assert(!harness.system->failures().empty());
        assert(harness.system->failures().back().error == EScriptSystemError::RESUME_QUEUE_FULL);
        assert(harness.system->executeStablePoint());
        assert(harness.backend_state.resumes == 0U);
        assert(harness.system->activeInstanceCount() == 0U);
    }

    void testResumeBudget()
    {
        HarnessOptions options;
        options.occurrence_capacity = 3U;
        options.limits = {8U, 1U, 3U, 3U, 3U, 3U, 64U, 1U, 3U, 3U, 3U, 3U};
        Harness harness{options};
        harness.recordBroadcastStart(1);
        harness.recordBroadcastStart(2);
        harness.recordBroadcastStart(3);
        assert(harness.broadcast_start.drain() == 3U);
        harness.recordBroadcastWait(14);
        assert(harness.broadcast_wait.drain() == 1U);
        assert(harness.system->stats().resume_queue_depth == 3U);
        assert(harness.system->executeStablePoint());
        assert(harness.backend_state.resumes == 1U);
        assert(harness.system->stats().resume_queue_depth == 2U);
        assert(harness.system->executeStablePoint());
        assert(harness.backend_state.resumes == 2U);
        assert(harness.system->executeStablePoint());
        assert(harness.backend_state.resumes == 3U);
    }

    void testPayloadFailures()
    {
        {
            Harness harness{{
                .disable_wait_copy = true,
                .expected_prepare_error = EScriptSystemError::SCRIPT_EVENT_SCHEMA_MISMATCH
            }};
        }
        {
            HarnessOptions options;
            options.limits = {8U, 1U, 2U, 2U, 2U, 2U, 1U, 2U, 2U, 2U, 2U, 2U};
            options.expected_prepare_error = EScriptSystemError::CAPACITY_EXCEEDED;
            Harness harness{options};
        }
        {
            Harness harness{{.fail_wait_copy = true}};
            harness.recordBroadcastStart(1);
            assert(harness.broadcast_start.drain() == 1U);
            harness.recordBroadcastWait(15);
            assert(harness.broadcast_wait.drain() == 1U);
            assert(harness.system->stats().active_event_waiters == 0U);
            assert(harness.system->executeStablePoint());
            assert(harness.backend_state.resumes == 0U);
            assert(harness.system->activeInstanceCount() == 0U);
        }
    }

    void testArtifactSchemaDriftFailsBeforeInstanceCreation()
    {
        constexpr std::array cases{
            std::pair{ERequirementMutation::EVENT_ID, EScriptSystemError::SCRIPT_EVENT_NOT_FOUND},
            std::pair{ERequirementMutation::SYSTEM_ID, EScriptSystemError::SCRIPT_EVENT_NOT_FOUND},
            std::pair{ERequirementMutation::ROUTE, EScriptSystemError::SCRIPT_EVENT_SCHEMA_MISMATCH},
            std::pair{ERequirementMutation::PAYLOAD_LAYOUT, EScriptSystemError::SCRIPT_EVENT_SCHEMA_MISMATCH},
            std::pair{ERequirementMutation::SCHEMA_HASH, EScriptSystemError::SCRIPT_EVENT_SCHEMA_MISMATCH},
            std::pair{ERequirementMutation::SCHEMA_VERSION, EScriptSystemError::SCRIPT_EVENT_SCHEMA_MISMATCH}
        };
        for (const auto& [mutation, expected] : cases)
        {
            HarnessOptions options;
            options.requirement_mutation = mutation;
            options.expected_prepare_error = expected;
            Harness harness{options};
            assert(harness.backend_state.instance_creates == 0U);
            assert(harness.backend_state.step_calls == 0U);
        }
    }

    void testNestedDispatch()
    {
        Harness harness{{
            .bind_callback = true,
            .callback_action = ECallbackAction::NESTED_DISPATCH,
            .immediate_wait_endpoint = true
        }};
        harness.recordBroadcastStart(1);
        assert(harness.broadcast_start.drain() == 1U);
        harness.emitImmediateWait(16);
        assert(harness.backend_state.callback_calls == 3U);
        assert(harness.system->stats().active_event_waiters == 0U);
        assert(harness.backend_state.resumes == 0U);
        assert(harness.system->executeStablePoint());
        assert(harness.backend_state.resumes == 1U);
        assert(harness.backend_state.resume_values == std::vector<std::int32_t>({16}));
    }

    void testShutdownWithPendingWaiter()
    {
        Harness harness{{}};
        harness.recordBroadcastStart(1);
        assert(harness.broadcast_start.drain() == 1U);
        assert(harness.system->stats().active_event_waiters == 1U);
        assert(harness.system->shutdown());
        const auto stats = harness.system->stats();
        assert(stats.active_event_waiters == 0U);
        assert(stats.active_awaitables == 0U);
        assert(stats.active_continuations == 0U);
        harness.recordBroadcastWait(17);
        assert(harness.broadcast_wait.drain() == 0U);
        assert(harness.backend_state.resumes == 0U);
    }

    void testIdleWaiterComplexity(std::size_t count)
    {
        HarnessOptions options;
        options.occurrence_capacity = count;
        options.limits = {16U, 1U, count, count, count, 1U, 64U, 1U, count, count, count, count};
        Harness harness{options};
        for (std::size_t index{}; index < count; ++index)
            harness.recordBroadcastStart(static_cast<std::int32_t>(index));
        assert(harness.broadcast_start.drain() == count);
        const auto before = harness.system->stats();
        assert(before.active_event_waiters == count);
        assert(harness.broadcast_wait.handlerCount() == 1U);
        for (std::size_t index{}; index < 4U; ++index)
            assert(harness.system->executeStablePoint());
        const auto after = harness.system->stats();
        assert(after.active_event_waiters == count);
        assert(after.event_waiter_dispatch_visits == before.event_waiter_dispatch_visits);
        assert(after.instance_cleanup_event_waiter_visits == before.instance_cleanup_event_waiter_visits);
        assert(after.instance_cleanup_awaitable_visits == before.instance_cleanup_awaitable_visits);
        assert(after.instance_cleanup_continuation_visits == before.instance_cleanup_continuation_visits);
        assert(harness.broadcast_wait.handlerCount() == 1U);
    }

    void testOutputSensitiveRetirement()
    {
        constexpr std::size_t kFirstInstanceWaiters{99'999U};
        HarnessOptions options;
        options.ownership_pair = true;
        options.occurrence_capacity = kFirstInstanceWaiters;
        options.limits = {
            16U,
            2U,
            100'000U,
            100'000U,
            100'000U,
            1U,
            64U,
            1U,
            100'000U,
            100'000U,
            100'000U,
            100'000U
        };
        Harness harness{options};
        for (std::size_t index{}; index < kFirstInstanceWaiters; ++index)
            harness.recordBroadcastStart(static_cast<std::int32_t>(index));
        assert(harness.broadcast_start.drain() == kFirstInstanceWaiters);
        harness.recordBroadcastStartSecond(1);
        assert(harness.broadcast_start_second.drain() == 1U);
        const auto before = harness.system->stats();
        assert(before.active_event_waiters == 100'000U);

        harness.backend_state.callback_action = ECallbackAction::FAIL;
        harness.recordBroadcastFaultSecond(1);
        assert(harness.broadcast_fault_second.drain() == 1U);
        assert(harness.system->executeStablePoint());
        const auto after = harness.system->stats();
        assert(after.active_event_waiters == kFirstInstanceWaiters);
        assert(after.instance_cleanup_event_waiter_visits - before.instance_cleanup_event_waiter_visits == 1U);
        assert(after.instance_cleanup_awaitable_visits - before.instance_cleanup_awaitable_visits == 1U);
        assert(after.instance_cleanup_continuation_visits - before.instance_cleanup_continuation_visits == 1U);
    }
}

int main()
{
    testBroadcastSemantics();
    testRegistrationCutoff();
    testTargetedAndRetirement();
    testTargetedScopeRejection();
    testCapacityFailure();
    testAwaitableCapacityFailure();
    testResumeQueueFailure();
    testResumeBudget();
    testPayloadFailures();
    testArtifactSchemaDriftFailsBeforeInstanceCreation();
    testNestedDispatch();
    testShutdownWithPendingWaiter();
    testIdleWaiterComplexity(10'000U);
    testIdleWaiterComplexity(50'000U);
    testIdleWaiterComplexity(100'000U);
    testOutputSensitiveRetirement();
    return 0;
}

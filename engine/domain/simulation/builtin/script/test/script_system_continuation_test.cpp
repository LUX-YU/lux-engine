#include "ScriptRuntimeTestRegion.hpp"
using lux::simulation::script::test::dispatchRuntimeHook;
using lux::simulation::script::test::deliverRuntimeEvent;
using lux::simulation::script::test::executeRuntimeStablePoint;
#if defined(LUX_SCRIPT_SOURCE_PROTOCOL_CLOCK)
#include "ScriptTestClock.hpp"
#endif
#include "../../../system/test/HookInvocationTestAccess.hpp"
using lux::simulation::test::dispatchHookForTest;
#include "../../../scripting/core/test/ScriptEndpointTestAccess.hpp"
using lux::simulation::script::test::deliverEndpoint;
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/abilities/DelayAbility.hpp>
#include "DelayAbility.ability.generated.hpp"
#include "TestAbility.hpp"
#include "TestAbility.ability.generated.hpp"

#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/scripting/ScriptAbilityInvocation.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace lux::simulation::script::test
{
    template <std::size_t Size, std::size_t Alignment = 1U>
    struct alignas(Alignment) ExternalPayload final
    {
        std::array<std::byte, Size> bytes{};
    };
}

namespace lux::semantic
{
    template <std::size_t Bytes, std::size_t Align>
    struct TypeTraits<lux::simulation::script::test::ExternalPayload<Bytes, Align>> final
    {
        using Value = lux::simulation::script::test::ExternalPayload<Bytes, Align>;
        inline static constexpr std::string_view CanonicalName = Bytes == 32U
            ? "lux.test.external.transport.payload32.with.a.deliberately.long.canonical.name"
            : Bytes == 33U ? "lux.test.external.payload33" : "lux.test.external.payload64";
        inline static constexpr std::uint8_t AbiKind = LUX_SCRIPT_VK_STRUCT_REF;
        inline static constexpr std::uint32_t Size = sizeof(Value);
        inline static constexpr std::uint32_t Alignment = alignof(Value);
    };
}

namespace
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    inline constexpr lux::system::SystemInstanceId kSystem{0x5101U};
    inline constexpr HookPointId kHook{0x5102U};
    inline constexpr HookPointId kHookSecond{0x5104U};
    inline constexpr HookPointId kHookThird{0x5105U};
    inline constexpr EventPointId kLargeEvent{0x5108U};
    using LargePayload = lux::simulation::script::test::ExternalPayload<64U>;
    inline constexpr lux::script::ScriptSymbolId kSymbol{0x5103U};
    inline constexpr lux::script::ScriptSymbolId kSymbolSecond{0x5106U};
    inline constexpr lux::script::ScriptSymbolId kSymbolThird{0x5107U};
    using TestAbility = lux::simulation::test::TestAbility;
    using TestAbilityTraits = lux::script::ScriptAbilityTraits<TestAbility>;
    using TestDispatch = TestAbilityTraits::Dispatch;
    inline constexpr auto kContract = TestAbilityTraits::Description.id;
    inline constexpr std::uint64_t kSchema = TestAbilityTraits::Description.schema_hash;

    enum class EAsyncProviderMode : std::uint8_t
    {
        REJECT,
        DELAYED,
        EAGER_SUCCESS,
        EAGER_FAILURE,
    };

    struct TestProvider final
    {
        explicit TestProvider(int& constructions, int& destructions) noexcept : destructions(&destructions)
        {
            ++constructions;
        }

        ~TestProvider()
        {
            ++*destructions;
        }

        int value{7};
        int calls{};
        int* destructions{};
        EAsyncProviderMode async_mode{EAsyncProviderMode::REJECT};
        std::optional<lux::script::ScriptAbilityCompletion<std::uint64_t>> pending;
        std::optional<lux::cxx::expected<void, lux::script::EScriptAbilityCompletionError>> eager_completion;

        int readValue(int input) noexcept
        {
            ++calls;
            return value + input;
        }

        void setValue(int new_value) noexcept
        {
            value = new_value;
        }

        std::uint64_t identity(std::uint64_t input) noexcept
        {
            return input;
        }

        const int& borrowedValue() noexcept
        {
            return value;
        }

        lux::script::ScriptAbilityStartResult beginOperation(
            std::uint64_t request,
            lux::script::ScriptAbilityCompletion<std::uint64_t> completion
        ) noexcept
        {
            switch (async_mode)
            {
            case EAsyncProviderMode::DELAYED:
                pending = std::move(completion);
                return {};
            case EAsyncProviderMode::EAGER_SUCCESS:
                eager_completion = completion.success(request + 1U);
                return {};
            case EAsyncProviderMode::EAGER_FAILURE:
                eager_completion = completion.fail({91});
                return {};
            case EAsyncProviderMode::REJECT:
                return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{81});
            }
            return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{82});
        }
    };

    [[nodiscard]] lux::asset::AssetId assetId()
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[0] = 0x51U;
        return lux::asset::AssetId{bytes};
    }



    [[nodiscard]] SimulationDescription makeSimulation()
    {
        constexpr std::array hooks{
            makeHookPointSpec<void()>(kHook, "tick"),
            makeHookPointSpec<void()>(kHookSecond, "tick-second"),
            makeHookPointSpec<void()>(kHookThird, "tick-third")
        };
        constexpr std::array events{makeEventPointSpec<LargePayload>(
            kLargeEvent, "large", kHook, EEventRoute::SIMULATION_BROADCAST,
            lux::semantic::TypeTraits<LargePayload>::CanonicalName, 1U
        )};
        const SimulationSystemDescription system{
            .type = {.canonical_name = "lux.test.script-continuation", .version = 1U},
            .hooks = hooks,
            .events = events};
        SimulationDescriptionBuilder builder;
        assert(builder.addSystem(kSystem, "script-continuation", system));
        auto result = std::move(builder).build();
        assert(result);
        return std::move(*result);
    }

    [[nodiscard]] lux::script::ScriptArtifact makeArtifact(bool require_capability)
    {
        lux::rdesc::Script description;
        description.module_name = "lux.test.script-continuation.fixture";
        description.exports.push_back({"tick", kSymbol, {}, {}});
        description.exports.push_back({"tick-second", kSymbolSecond, {}, {}});
        description.exports.push_back({"tick-third", kSymbolThird, {}, {}});
        if (require_capability)
        {
            description.api_requirements.push_back({lux::script::ScriptApiContractId{kContract.name()}, kSchema});
        }
        description.body = lux::rdesc::CppStaticScript{"fixture"};
        auto result = lux::script::ScriptArtifact::create(std::move(description), {});
        assert(result);
        return std::move(*result);
    }

    struct BackendState;

    struct BackendInstance final
    {
        BackendState* owner{};
        void* provider{};
        const TestDispatch* dispatch{};
    };

    struct PreparedSync final
    {
        BackendInstance* instance{};
    };

    struct ContinuationState final
    {
        BackendState* owner{};
        std::size_t suspensions_remaining{};
    };

    struct BackendState final
    {
        ScriptEventAdmissionHandle event;
        std::optional<lux::script::ScriptAbilityStarter<DelayAbility>> delay;
        std::vector<lux::script::ScriptAbilityCompletion<void>> timer_completions;
        bool simulation_timer{};
        double timer_seconds{1000.0};
        bool check_timer_errors{true};
        std::vector<ScriptInstanceId> resumed_instances;
        bool discard_timer{};
        bool reject_after_timer{};
        bool enable_step{};
        bool enable_ability_async{};
        bool eager_first{};
        bool eager_resuspend{};
        bool typed_result{};
        std::size_t suspensions_after_first{};
        std::size_t creates{};
        std::size_t destroys{};
        std::size_t sync_calls{};
        std::size_t step_calls{};
        std::size_t resume_calls{};
        std::size_t continuation_destroys{};
        std::size_t capability_bind_scans{};
        std::size_t resume_depth{};
        std::size_t max_resume_depth{};
        bool saw_typed_result{};
        bool saw_failure{};
        std::int32_t failure_status{};
        std::uint64_t ability_result{};
        std::thread::id resume_thread;
        std::vector<ScriptAwaitableCompletion> completions;
        std::optional<lux::script::ScriptAbilityStarter<TestAbility>> ability_starter;
        ScriptStepResult (*custom_step)(BackendState&, ScriptStepContext&) noexcept{};
        std::size_t provider_starts{};
        std::size_t expected_payload_size{};
        void* resume_probe_context{};
        void (*resume_probe)(void*) noexcept{};
        void* destroy_probe_context{};
        void (*destroy_probe)(void*) noexcept{};
        std::size_t blocked_instances{};
    };

    int invokeSync(lux_script_call_frame* frame)
    {
        auto& call = *static_cast<PreparedSync*>(frame->user_context);
        ++call.instance->owner->sync_calls;
        if (call.instance->dispatch != nullptr)
        {
            const int result = call.instance->dispatch->readValue(call.instance->provider, 5);
            return result == 12 ? 0 : 91;
        }
        return 0;
    }

    EScriptBackendResult createInstance(void* context,
                                        const ScriptInstanceCreateContext& create,
                                        const lux::script::ScriptArtifact&,
                                        ScriptBackendInstance& output) noexcept
    {
        auto& state = *static_cast<BackendState*>(context);
        auto* instance = new (std::nothrow) BackendInstance();
        if (instance == nullptr)
            return EScriptBackendResult::ALLOCATION_FAILURE;
        instance->owner = &state;
        if (!create.events.empty()) state.event = create.events.front().admission;
        ++state.capability_bind_scans;
        const auto delay_id = lux::script::ScriptAbilityTraits<DelayAbility>::Description.id;
        if (!create.capabilities.empty() && create.capabilities.front().contract.name() == delay_id.name())
        {
            const auto& capability = create.capabilities.front();
            auto starter = lux::script::ScriptAbilityStarter<DelayAbility>::create({
                &lux::script::ScriptAbilityTraits<DelayAbility>::Description, capability.context, capability.dispatch
            });
            assert(starter);
            state.delay = std::move(*starter);
        }
        else if (!create.capabilities.empty())
        {
            assert(create.capabilities.size() == 1U);
            instance->provider = create.capabilities.front().context;
            instance->dispatch = static_cast<const TestDispatch*>(create.capabilities.front().dispatch);
            const lux::script::ScriptAbilityBinding binding{
                &TestAbilityTraits::Description,
                instance->provider,
                instance->dispatch
            };
            auto starter = lux::script::ScriptAbilityStarter<TestAbility>::create(binding);
            if (!starter)
            {
                delete instance;
                return EScriptBackendResult::CONSTRUCTION_FAILURE;
            }
            state.ability_starter = std::move(*starter);
        }
        ++state.creates;
        output.value = instance;
        return EScriptBackendResult::SUCCESS;
    }

    ScriptStepResult invokeStep(
        void* context,
        lux_script_call_frame&,
        ScriptStepContext& step,
        ScriptBackendContinuation& output
    ) noexcept;

    EScriptBackendResult prepareMethod(void*,
                                       ScriptBackendInstance instance,
                                       const lux::rdesc::ScriptFunction&,
                                       ScriptBackendPreparedMethod& output) noexcept
    {
        auto* prepared = new (std::nothrow) PreparedSync();
        if (prepared == nullptr)
            return EScriptBackendResult::ALLOCATION_FAILURE;
        prepared->instance = static_cast<BackendInstance*>(instance.value);
        auto* state = prepared->instance->owner;
        output = {
            prepared,
            lux::script::BoundScriptCall{&invokeSync, prepared},
            state->enable_step ? BoundScriptStepCall{state, &invokeStep} : BoundScriptStepCall{}
        };
        return EScriptBackendResult::SUCCESS;
    }

    void releaseMethod(void*, ScriptBackendInstance, ScriptBackendPreparedMethod method) noexcept
    {
        delete static_cast<PreparedSync*>(method.token);
    }

    void destroyInstance(void*, ScriptBackendInstance instance) noexcept
    {
        auto* object = static_cast<BackendInstance*>(instance.value);
        ++object->owner->destroys;
        delete object;
    }

    void destroyContinuation(void* value) noexcept
    {
        auto* continuation = static_cast<ContinuationState*>(value);
        ++continuation->owner->continuation_destroys;
        if (const auto probe = std::exchange(continuation->owner->destroy_probe, nullptr))
            probe(continuation->owner->destroy_probe_context);
        delete continuation;
    }

    ScriptStepResult resumeContinuation(void* value,
                                        ScriptStepContext& context,
                                        const ScriptResumePacket& packet) noexcept
    {
        auto& continuation = *static_cast<ContinuationState*>(value);
        auto& owner = *continuation.owner;
        if (owner.resume_probe != nullptr)
            owner.resume_probe(owner.resume_probe_context);
        if (packet.state == EScriptAwaitableState::FAILED)
        {
            assert(packet.error.valid());
            owner.saw_failure = true;
            owner.failure_status = packet.error.status;
        }
        else
        {
            assert(packet.state == EScriptAwaitableState::READY);
            if (owner.expected_payload_size != 0U)
            {
                assert(packet.value != nullptr && packet.value->bytes.size() == owner.expected_payload_size);
            }
            if (owner.enable_ability_async)
            {
                assert(packet.value != nullptr && packet.value->type.valid());
                assert(packet.value->type.type_id == lux::semantic::typeId("lux.u64"));
                assert(packet.value->bytes.size() == sizeof(std::uint64_t));
                std::memcpy(
                    std::addressof(owner.ability_result),
                    packet.value->bytes.data(),
                    sizeof(owner.ability_result)
                );
            }
            else if (owner.typed_result)
            {
                assert(packet.value != nullptr && packet.value->type.valid());
                assert(packet.value->type.type_id == lux::semantic::typeId("lux.i32"));
                assert(packet.value->bytes.size() == sizeof(std::int32_t));
                owner.saw_typed_result = true;
            }
        }
        if (owner.delay)
            owner.resumed_instances.push_back(context.instance);
        ++owner.resume_calls;
        owner.resume_thread = std::this_thread::get_id();
        ++owner.resume_depth;
        owner.max_resume_depth = (std::max)(owner.max_resume_depth, owner.resume_depth);

        ScriptStepResult result = ScriptStepResult::completed();
        if (continuation.suspensions_remaining != 0U)
        {
            --continuation.suspensions_remaining;
            auto awaiting = context.awaitables.create(
                owner.typed_result ? std::optional{lux::rdesc::makeScriptValueType<std::int32_t>()} : std::nullopt);
            assert(awaiting);
            owner.completions.push_back(awaiting->completion);
            if (owner.eager_resuspend)
                assert(awaiting->completion.ready());
            result = ScriptStepResult::suspended(awaiting->id);
        }
        --owner.resume_depth;
        return result;
    }

    ScriptStepResult invokeStep(void* context,
                                lux_script_call_frame&,
                                ScriptStepContext& step,
                                ScriptBackendContinuation& output) noexcept
    {
        auto& state = *static_cast<BackendState*>(context);
        ++state.step_calls;
        ScriptStepResult result;
        if (state.custom_step != nullptr)
        {
            result = state.custom_step(state, step);
        }
        else if (state.enable_ability_async)
        {
            assert(state.ability_starter.has_value());
            result = invokeScriptAbilityAsync<std::uint64_t>(
                step,
                [&state](lux::script::ScriptAbilityCompletion<std::uint64_t> completion) noexcept {
                    return state.ability_starter->beginOperation(41U, std::move(completion));
                }
            );
        }
        else
        {
            auto awaiting = step.awaitables.create(
                state.typed_result ? std::optional{lux::rdesc::makeScriptValueType<std::int32_t>()} : std::nullopt
            );
            if (!awaiting)
                return ScriptStepResult::failed(71);
            state.completions.push_back(awaiting->completion);
            if (state.eager_first)
                assert(awaiting->completion.ready());
            result = ScriptStepResult::suspended(awaiting->id);
        }
        if (result.state != EScriptStepState::SUSPENDED)
            return result;
        auto* continuation = new (std::nothrow) ContinuationState();
        if (continuation == nullptr)
            return ScriptStepResult::failed(72);
        continuation->owner = &state;
        continuation->suspensions_remaining = state.suspensions_after_first;
        output = {continuation, &resumeContinuation, &destroyContinuation};
        return result;
    }

    struct Harness final
    {
        explicit Harness(
            bool require_capability,
            std::size_t mount_count = 1U,
            bool entity_scope = false,
            bool quota_layout = false,
            bool owner_large_result = false
        )
            : simulation(makeSimulation()), artifact(makeArtifact(require_capability)), asset(assetId()),
                uses_entity_scope(entity_scope)
        {
            if (owner_large_result)
            {
                auto source = artifact.description();
                using Traits = lux::semantic::TypeTraits<LargePayload>;
                source.event_requirements.push_back({
                    "script-continuation", "large", kSystem.value, kLargeEvent.value,
                    lux::script::EScriptEventRoute::SIMULATION_BROADCAST,
                    {std::string{Traits::CanonicalName}, lux::semantic::typeId(Traits::CanonicalName),
                        Traits::AbiKind, Traits::Size, Traits::Alignment},
                    lux::semantic::typeId(Traits::CanonicalName), 1U,
                    kHook.value, simulation.findHookPoint(kSystem, kHook).contractHash(), 1U
                });
                auto updated = lux::script::ScriptArtifact::create(std::move(source), {});
                assert(updated);
                artifact = std::move(*updated);
                assert(large_event.prepare({1U, 2U}, [](const LargePayload& value) noexcept {
                    return LargePayload{value.bytes};
                }) == EEndpointMutationError::NONE);
                large_bridge = std::make_unique<ScriptEventEndpoint<SimulationBroadcastRoute, LargePayload>>(
                    kSystem, kLargeEvent, large_event,
                    [](const LargePayload& payload, std::span<std::byte> bytes) noexcept {
                        if (bytes.size() != sizeof(payload))
                            return false;
                        std::memcpy(bytes.data(), &payload, sizeof(payload));
                        return true;
                    });
                large_endpoint = large_bridge->descriptor();
            }
            if (uses_entity_scope)
                entity = registry.create();
            std::vector<ScriptRuntimeMount> builder;
            if (quota_layout)
            {
                assert(mount_count == 2U && !uses_entity_scope);
                builder.push_back({ScriptMountId{1U}, asset, SimulationScriptScope{}, {
                        {kSymbol, HookScriptTarget{kSystem, kHook}},
                        {kSymbolSecond, HookScriptTarget{kSystem, kHookSecond}}
                    }});
                builder.push_back({ScriptMountId{2U}, asset, SimulationScriptScope{}, {{kSymbolThird,
                    HookScriptTarget{kSystem, kHookThird}}}});
            }
            else
            {
                for (std::size_t index{}; index < mount_count; ++index)
                {
                    builder.push_back({ScriptMountId{index + 1U}, asset,
                        uses_entity_scope ? ScriptInstanceScope{EntityScriptScope{entity}}
                                                               : ScriptInstanceScope{SimulationScriptScope{}},
                                                                   {{kSymbol, HookScriptTarget{kSystem, kHook}}}});
                }
            }
            auto built = std::optional{std::move(builder)};
            assert(built);
            description = std::move(*built);
            assert(hook.prepare(1U) == EEndpointMutationError::NONE);
            assert(hook_second.prepare(1U) == EEndpointMutationError::NONE);
            assert(hook_third.prepare(1U) == EEndpointMutationError::NONE);
            bridge = std::make_unique<ScriptHookEndpoint<void()>>(kSystem, kHook, hook);
            bridge_second =
                std::make_unique<ScriptHookEndpoint<void()>>(kSystem, kHookSecond, hook_second);
            bridge_third = std::make_unique<ScriptHookEndpoint<void()>>(kSystem, kHookThird, hook_third);
            endpoints = {bridge->descriptor(), bridge_second->descriptor(), bridge_third->descriptor()};
            backend = {lux::rdesc::Script::Kind::CPP_STATIC,
                       &backend_state,
                       &createInstance,
                       &prepareMethod,
                       &releaseMethod,
                       &destroyInstance};
        }

        [[nodiscard]] lux::cxx::expected<ScriptSystem, EScriptSystemError> create(
            ScriptRuntimeLimits limits,
            std::span<const ScriptApiCapabilityPublication> capabilities,
            bool include_endpoint = true,
            const SimulationClock* clock_override = nullptr) noexcept
        {
            return ScriptSystem::create(
                simulation,
                *planScriptRuntimeCapacity(description),
                description,
                registry,
                clock_override ? *clock_override : clock,
                limits,
                {this, &resolveArtifact},
                capabilities,
                std::span{&backend, 1U},
                include_endpoint ? std::span<const ScriptHookEndpointDescriptor>{endpoints}
                : std::span<const ScriptHookEndpointDescriptor>{},
                large_bridge ? std::span{&large_endpoint, 1U} : std::span<const ScriptEventEndpointDescriptor>{}
            );
        }

        static bool resolveArtifact(void* context,
                                    const lux::asset::AssetId& requested,
                                    ResolvedScriptArtifact& output) noexcept
        {
            auto& self = *static_cast<Harness*>(context);
            if (requested != self.asset)
                return false;
            output.artifact = &self.artifact;
            return true;
        }



        SimulationDescription simulation;
        SimulationClock clock;
        std::vector<ScriptRuntimeMount> description;
        lux::script::ScriptArtifact artifact;
        lux::asset::AssetId asset;
        ecs::Registry registry;
        ecs::Entity entity{ecs::NullEntity};
        bool uses_entity_scope{};
        HookPoint<void()> hook;
        HookPoint<void()> hook_second;
        HookPoint<void()> hook_third;
        std::unique_ptr<ScriptHookEndpoint<void()>> bridge;
        std::unique_ptr<ScriptHookEndpoint<void()>> bridge_second;
        std::unique_ptr<ScriptHookEndpoint<void()>> bridge_third;
        std::array<ScriptHookEndpointDescriptor, 3U> endpoints;
        BackendState backend_state;
        ScriptBackendDescriptor backend;
        HookChannel<SimulationBroadcastRoute, LargePayload> large_event;
        std::unique_ptr<ScriptEventEndpoint<SimulationBroadcastRoute, LargePayload>> large_bridge;
        ScriptEventEndpointDescriptor large_endpoint;
    };

    [[nodiscard]] ScriptRuntimeLimits limits(std::size_t instances = 1U,
                                             std::size_t continuations = 8U,
                                             std::size_t awaitables = 8U,
                                             std::size_t resumes = 8U,
                                             std::size_t budget = 8U,
                                             std::size_t continuations_per_instance = 0U,
                                             std::size_t external_completions = 0U) noexcept
    {
        const std::size_t per_instance = continuations_per_instance == 0U
            ? continuations
            : continuations_per_instance;
        const std::size_t external_capacity = external_completions == 0U ? awaitables : external_completions;
        return {
            16U,
            instances,
            continuations,
            per_instance,
            awaitables,
            resumes,
            64U,
            budget,
            awaitables,
            awaitables,
            awaitables,
            external_capacity
        };
    }

    void configureTimerHarness(Harness& harness)
    {
        auto source = harness.artifact.description();
        const auto& delay = lux::script::ScriptAbilityTraits<DelayAbility>::Description;
        source.api_requirements.push_back({lux::script::ScriptApiContractId{delay.id.name()}, delay.schema_hash});
        auto artifact = lux::script::ScriptArtifact::create(std::move(source), {});
        assert(artifact);
        harness.artifact = std::move(*artifact);
        auto& backend = harness.backend_state;
        backend.enable_step = true;
        backend.custom_step = [](BackendState& state, ScriptStepContext& context) noexcept {
            if (std::exchange(state.check_timer_errors, false))
            {
                const std::array bad_durations{-1.0, std::numeric_limits<double>::quiet_NaN(),
                    std::numeric_limits<double>::infinity(), (std::numeric_limits<double>::max)()};
                for (std::size_t index{}; index < bad_durations.size(); ++index)
                {
                    const auto rejected = invokeScriptAbilityAsync<void>(context,
                        [&](lux::script::ScriptAbilityCompletion<void> completion) noexcept {
                            return state.delay->simulationSeconds(bad_durations[index], std::move(completion));
                        });
                    const auto expected = index == 3U ? EScriptDelayStatus::DURATION_OVERFLOW :
                        EScriptDelayStatus::INVALID_DURATION;
                    assert(rejected.state == EScriptStepState::FAILED);
                    assert(rejected.error.status == static_cast<std::int32_t>(expected));
                }
            }
            auto result = invokeScriptAbilityAsync<void>(context,
                [&state](lux::script::ScriptAbilityCompletion<void> completion) noexcept {
                    state.timer_completions.push_back(completion);
                    auto started = state.simulation_timer
                        ? state.delay->simulationSeconds(state.timer_seconds, completion)
                        : state.delay->nextStep(completion);
                    if (started && state.reject_after_timer)
                        return lux::script::ScriptAbilityStartResult{
                            lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{87})};
                    return started;
                });
            if (state.discard_timer && result.state == EScriptStepState::SUSPENDED)
            {
                context.awaitables.discard(result.waiting_on);
                // Idempotent cancellation cannot remove another source.
                context.awaitables.discard(result.waiting_on);
                return ScriptStepResult::completed();
            }
            return result;
        };
    }

    void testTimerSourceCancellation()
    {
        for (const bool simulation_timer : {false, true})
        {
            Harness harness{false, 2U, false, true};
            const auto other = harness.registry.create();
            auto entity = harness.registry.create();
            harness.description[0].scope = EntityScriptScope{entity};
            harness.description[1].scope = EntityScriptScope{other};
            configureTimerHarness(harness);
            auto& backend = harness.backend_state;
            backend.simulation_timer = simulation_timer;
            auto configured = limits(2U, 4U, 4U);
            configured.next_step_wait_capacity = 2U;
            configured.simulation_delay_capacity = 2U;
            auto created = harness.create(configured, {});
            assert(created && created->prepare());
            auto& system = *created;
            const auto waits = [&]() noexcept {
                const auto stats = system.stats();
                return stats.next_step_waits + stats.simulation_delay_waits;
            };
            std::array<ScriptMountStatus, 2U> changes;
            assert(system.collectMountStatusChanges(changes));
            assert(dispatchRuntimeHook(system, harness.hook_third) == 1U);
            assert(waits() == 1U && system.activeContinuationCount() == 1U);
            const auto other_completion = backend.timer_completions.front();
            backend.discard_timer = true;
            for (std::size_t repeat{}; repeat < 32U; ++repeat)
            {
                assert(dispatchRuntimeHook(system, harness.hook) == 1U);
                assert(waits() == 1U && system.activeAwaitableCount() == 1U);
                assert(other_completion.active());
                assert(!backend.timer_completions.back().active());
            }
            backend.discard_timer = false;
            for (std::size_t repeat{}; repeat < 32U; ++repeat)
            {
                assert(dispatchRuntimeHook(system, harness.hook) == 1U);
                assert(waits() == 2U && system.activeContinuationCount() == 2U);
                const auto old_completion = backend.timer_completions.back();
                harness.registry.destroy(entity);
                // Physical Timer capacity is returned by logical retirement, without advancing the real step.
                assert(waits() == 1U && system.activeAwaitableCount() == 1U);
                assert(!old_completion.active() && other_completion.active());
                assert(system.processLifecycle());
                assert(backend.continuation_destroys == repeat + 1U);
                assert(system.collectMountStatusChanges(changes));
                entity = harness.registry.create();
                harness.description[0].scope = EntityScriptScope{entity};
                assert(system.mountResolvedBatch(std::span{harness.description.data(), 1U}));
                assert(system.processLifecycle());
                assert(system.collectMountStatusChanges(changes));
                assert(waits() == 1U && system.activeInstanceCount() == 2U);
                const auto stale = old_completion.success();
                assert(!stale && stale.error() == lux::script::EScriptAbilityCompletionError::STALE);
                assert(other_completion.active() && waits() == 1U);
            }
            assert(backend.resume_calls == 0U && system.failures().empty());
            // An external terminal result also unlinks the Timer source before its deadline.
            assert(other_completion.success());
            assert(waits() == 1U && backend.resume_calls == 0U);
            assert(executeRuntimeStablePoint(system));
            assert(waits() == 0U && backend.resume_calls == 1U);
            assert(dispatchRuntimeHook(system, harness.hook_third) == 1U);
            assert(waits() == 1U);
            backend.reject_after_timer = true;
            assert(dispatchRuntimeHook(system, harness.hook) == 1U);
            assert(waits() == 1U && system.activeAwaitableCount() == 1U);
            assert(system.failures().size() == 1U && system.failures().front().status == 87);
            assert(system.processLifecycle());
            assert(system.shutdown());
            assert(waits() == 0U && system.activeAwaitableCount() == 0U);
            assert(backend.continuation_destroys == 34U && backend.creates == backend.destroys);
            assert(backend.resume_calls == 1U);
        }
        Harness bounded{false};
        auto oversized = limits();
        oversized.next_step_wait_capacity = (std::numeric_limits<std::size_t>::max)();
        oversized.simulation_delay_capacity = oversized.next_step_wait_capacity;
        // Physical Timer storage is bounded by the shared result capacity; summing logical caps must not overflow.
        auto bounded_runtime = bounded.create(oversized, {});
        assert(bounded_runtime && bounded_runtime->prepare() && bounded_runtime->shutdown());
    }

#if defined(LUX_SCRIPT_SOURCE_PROTOCOL_CLOCK)
    void testTimerDeadlineOrderAndBackpressure()
    {
        for (const unsigned mode : {0U, 1U, 2U}) // NextStep, zero simulation delay, positive delay.
        {
            Harness harness{false, 2U, false, true};
            lux::simulation::script::test::ScriptTestClock clock_owner{harness.registry};
            configureTimerHarness(harness);
            auto& backend = harness.backend_state;
            backend.simulation_timer = mode != 0U;
            backend.timer_seconds = mode == 2U ? 1.0 : 0.0;
            auto configured = limits(2U, 4U, 4U, 1U, 1U);
            auto created = harness.create(configured, {}, true, &clock_owner.clock());
            assert(created && created->prepare());
            auto& system = *created;
            const auto first = (**system.queryMountStatus({1U})).instance;
            const auto second = (**system.queryMountStatus({2U})).instance;
            assert(dispatchRuntimeHook(system, harness.hook) == 1U);
            assert(dispatchRuntimeHook(system, harness.hook_third) == 1U);
            assert(system.stats().next_step_waits + system.stats().simulation_delay_waits == 2U);
            assert(backend.step_calls == 2U);
            if (mode == 2U)
            {
                clock_owner.advance(SimulationDuration{});
                assert(executeRuntimeStablePoint(system));
                assert(backend.resume_calls == 0U && system.stats().simulation_delay_waits == 2U);
            }
            clock_owner.advance(mode == 2U ? SimulationDuration{1'000'000'000} : SimulationDuration{});
            assert(executeRuntimeStablePoint(system));
            assert(backend.resumed_instances == std::vector<ScriptInstanceId>{first});
            assert(system.stats().next_step_waits + system.stats().simulation_delay_waits == 1U);
            assert(backend.timer_completions.back().active()); // Second source hit real ResumeRing backpressure.
            assert(executeRuntimeStablePoint(system));
            assert(backend.resume_calls == 1U); // No Timer retry in a duplicate stable point or after a pop.
            const auto due_step = clock_owner.clock().snapshot().step_index;
            clock_owner.advance(SimulationDuration{});
            assert(executeRuntimeStablePoint(system));
            assert(clock_owner.clock().snapshot().step_index == due_step + 1U);
            assert(backend.resumed_instances == std::vector<ScriptInstanceId>({first, second}));
            assert(system.stats().next_step_waits + system.stats().simulation_delay_waits == 0U);
            assert(system.activeAwaitableCount() == 0U && system.failures().empty());
            assert(system.shutdown() && backend.continuation_destroys == 2U);
            std::printf("TIMER_RETRY,mode=%u,due_step=%llu,retry_step=%llu,resumes=2,waits=0\n",
                mode, due_step, clock_owner.clock().snapshot().step_index);
        }
    }
#endif

    void testCapabilities()
    {
        int constructions{}, destructions{};
        {
            TestProvider provider{constructions, destructions};
            const auto ability = lux::script::bindScriptAbility<TestAbility>(provider);
            const std::array publication{publishScriptAbility(ability)};
            Harness available{true};
            auto created = available.create(limits(), publication);
            assert(created);
            auto system = std::move(*created);
            assert(system.prepare());
            assert(dispatchRuntimeHook(system, available.hook) == 1U);
            assert(dispatchRuntimeHook(system, available.hook) == 1U);
            assert(provider.calls == 2);
            assert(available.backend_state.capability_bind_scans == 1U);
            assert(system.shutdown());
            assert(destructions == 0);
        }
        assert(constructions == 1 && destructions == 1);

        Harness missing{true};
        auto missing_created = missing.create(limits(), {});
        assert(missing_created);
        auto missing_system = std::move(*missing_created);
        const auto missing_prepared = missing_system.prepare();
        assert(!missing_prepared && missing_prepared.error() == EScriptSystemError::SCRIPT_CAPABILITY_NOT_FOUND);
        assert(missing.backend_state.creates == 0U);
        assert(missing_system.shutdown());

        TestProvider mismatch_provider{constructions, destructions};
        auto mismatch_capability = publishScriptAbility(lux::script::bindScriptAbility<TestAbility>(mismatch_provider));
        ++mismatch_capability.schema_hash;
        const std::array mismatch_publication{mismatch_capability};
        Harness mismatch{true};
        auto mismatch_created = mismatch.create(limits(), mismatch_publication);
        assert(mismatch_created);
        auto mismatch_system = std::move(*mismatch_created);
        const auto mismatch_prepared = mismatch_system.prepare();
        assert(!mismatch_prepared &&
               mismatch_prepared.error() == EScriptSystemError::SCRIPT_CAPABILITY_SCHEMA_MISMATCH);
        assert(mismatch.backend_state.creates == 0U);
        assert(mismatch_system.shutdown());

        const auto duplicate_capability =
            publishScriptAbility(lux::script::bindScriptAbility<TestAbility>(mismatch_provider));
        const std::array ambiguous{duplicate_capability, duplicate_capability};
        Harness duplicate{false};
        const auto duplicate_created = duplicate.create(limits(), ambiguous);
        assert(!duplicate_created &&
               duplicate_created.error() == EScriptSystemError::SCRIPT_CAPABILITY_AMBIGUOUS_PROVIDER);

        Harness endpoint_missing{false};
        auto endpoint_created = endpoint_missing.create(limits(), {}, false);
        assert(!endpoint_created && endpoint_created.error() == EScriptSystemError::SCRIPT_ENDPOINT_NOT_FOUND);
        assert(endpoint_missing.backend_state.creates == 0U);
    }

    void testSingleFlightIsolation()
    {
        Harness harness{false, 2U, false, true};
        auto& state = harness.backend_state;
        state.enable_step = true;
        auto created = harness.create(limits(2U, 8U, 8U, 8U, 1U), {});
        assert(created && created->prepare());
        auto& system = *created;
        assert(dispatchRuntimeHook(system, harness.hook) == 1U);
        for (std::size_t count{}; count < 16U; ++count)
            assert(dispatchRuntimeHook(system, harness.hook) == 1U);
        assert(state.step_calls == 1U && state.completions.size() == 1U);
        assert(system.activeContinuationCount() == 1U && system.activeAwaitableCount() == 1U);
        assert(dispatchRuntimeHook(system, harness.hook_second) == 1U); // Different method of the same instance.
        assert(dispatchRuntimeHook(system, harness.hook_third) == 1U); // Different instance.
        assert(state.step_calls == 3U && state.completions.size() == 3U);
        const auto old_completion = state.completions.front();
        assert(old_completion.ready());
        assert(executeRuntimeStablePoint(system));
        assert(state.resume_calls == 1U && state.continuation_destroys == 1U);
        assert(dispatchRuntimeHook(system, harness.hook) == 1U);
        assert(state.step_calls == 4U && system.activeContinuationCount() == 3U);
        assert(!old_completion.ready());
        assert(system.failures().empty() && system.shutdown());
        assert(state.continuation_destroys == 4U && state.destroys == 2U);

        Harness reused{false, 1U, true};
        reused.backend_state.enable_step = true;
        auto replacement = reused.create(limits(), {});
        assert(replacement && replacement->prepare());
        assert(dispatchRuntimeHook(*replacement, reused.hook) == 1U);
        const auto old_status = replacement->queryMountStatus(reused.description[0].id);
        assert(old_status && old_status->has_value());
        const auto old_id = (*old_status)->instance;
        const auto retired_completion = reused.backend_state.completions.front();
        reused.registry.destroy(reused.entity);
        assert(replacement->processLifecycle());
        std::array<ScriptMountStatus, 1U> feedback;
        assert(replacement->collectMountStatusChanges(feedback));
        reused.entity = reused.registry.create();
        reused.description[0].scope = EntityScriptScope{reused.entity};
        assert(replacement->mountResolvedBatch(reused.description));
        assert(replacement->processLifecycle());
        const auto new_status = replacement->queryMountStatus(reused.description[0].id);
        assert(new_status && new_status->has_value());
        const auto new_id = (*new_status)->instance;
        assert(new_id.slot == old_id.slot && new_id.generation != old_id.generation);
        assert(!retired_completion.ready());
        assert(dispatchRuntimeHook(*replacement, reused.hook) == 1U);
        assert(reused.backend_state.step_calls == 2U && replacement->activeContinuationCount() == 1U);
        assert(replacement->failures().empty() && replacement->shutdown());
        assert(reused.backend_state.continuation_destroys == 2U && reused.backend_state.destroys == 2U);
        std::puts("SINGLE_FLIGHT_OK,skipped=16,methods=2,instances=2,resume_budget=1,reused_generation=1");
    }

    void testSharedHookMethod()
    {
        Harness harness{false};
        harness.description[0].bindings.push_back({kSymbol, HookScriptTarget{kSystem, kHookSecond}});
        auto& state = harness.backend_state;
        state.enable_step = true;
        auto created = harness.create(limits(), {});
        assert(created && created->prepare());
        assert(dispatchRuntimeHook(*created, harness.hook) == 1U);
        assert(dispatchRuntimeHook(*created, harness.hook_second) == 1U && state.step_calls == 1U);
        assert(state.completions.front().ready());
        assert(dispatchRuntimeHook(*created, harness.hook_second) == 1U && state.step_calls == 1U);
        struct Probe { ScriptSystem* system; Harness* harness; } probe{&*created, &harness};
        state.destroy_probe_context = &probe;
        state.destroy_probe = [](void* context) noexcept {
            const auto& value = *static_cast<Probe*>(context);
            assert(dispatchRuntimeHook(*value.system, value.harness->hook_second) == 1U);
            assert(value.harness->backend_state.step_calls == 2U);
        };
        assert(executeRuntimeStablePoint(*created));
        assert(state.resume_calls == 1U && state.continuation_destroys == 1U);
        assert(created->activeContinuationCount() == 1U && state.step_calls == 2U);
        assert(dispatchRuntimeHook(*created, harness.hook) == 1U && state.step_calls == 2U);
        assert(created->shutdown() && state.continuation_destroys == 2U && state.destroys == 1U);
        std::puts("HOOK_SHARED_METHOD_OK,aliases=2,ready_still_blocked=1,destroy_reentry=1,destroys=2");
    }

    void testHookRatios()
    {
        constexpr std::size_t count = 1000U, batches = 64U;
        for (const auto percent : {0U, 50U, 99U, 100U})
        {
            Harness harness{false, count};
            auto& state = harness.backend_state;
            state.enable_step = true;
            state.blocked_instances = count * percent / 100U;
            state.custom_step = [](BackendState& owner, ScriptStepContext& step) noexcept {
                if (step.instance.slot > owner.blocked_instances)
                    return ScriptStepResult::completed();
                const auto awaiting = step.awaitables.create(std::nullopt);
                assert(awaiting);
                owner.completions.push_back(awaiting->completion);
                return ScriptStepResult::suspended(awaiting->id);
            };
            auto created = harness.create(limits(count, count, count, count, 1U), {});
            assert(created && created->prepare());
            assert(dispatchRuntimeHook(*created, harness.hook) == 1U);
            const auto before = created->stats();
            for (std::size_t batch{}; batch < batches; ++batch)
                assert(dispatchRuntimeHook(*created, harness.hook) == 1U);
            const auto after = created->stats();
            const auto starts = (count - state.blocked_instances) * batches;
            assert(state.step_calls == count + starts && after.active_continuations == state.blocked_instances);
            if (after.hook_observation_enabled)
            {
                assert(after.hook_candidates - before.hook_candidates == count * batches);
                assert(after.hook_handler_visits - before.hook_handler_visits == starts);
            }
            std::printf("HOOK_RATIO percent=%u batches=%zu population=%zu new_calls=%zu resume=0 waiting=%zu "
                "backlog=0 observed=%u candidates=%llu visits=%llu\n", percent, batches, count, starts,
                state.blocked_instances, after.hook_observation_enabled,
                after.hook_candidates - before.hook_candidates, after.hook_handler_visits - before.hook_handler_visits);
            for (auto& completion : state.completions) assert(completion.ready());
            assert(created->stats().resume_queue_depth == state.blocked_instances);
            while (created->stats().resume_queue_depth != 0U) assert(executeRuntimeStablePoint(*created));
            assert(state.resume_calls == state.blocked_instances &&
                state.continuation_destroys == state.blocked_instances && created->failures().empty());
            assert(created->shutdown() && state.destroys == count);
        }
    }

    void testNonPowerOfTwoResumeWrap()
    {
        Harness harness{false, 3U};
        auto& state = harness.backend_state;
        state.enable_step = true;
        auto created = harness.create(limits(3U, 3U, 3U, 3U, 2U), {});
        assert(created && created->prepare());
        std::size_t completed{};
        for (std::size_t cycle{}; cycle < 17U; ++cycle)
        {
            assert(dispatchRuntimeHook(*created, harness.hook) == 1U);
            while (completed < state.completions.size())
                assert(state.completions[completed++].ready());
            assert(executeRuntimeStablePoint(*created));
            assert(state.resume_calls == (cycle + 1U) * 2U);
            assert(created->stats().resume_queue_depth == 1U);
        }
        assert(executeRuntimeStablePoint(*created));
        assert(state.step_calls == 35U && state.resume_calls == 35U && state.continuation_destroys == 35U);
        assert(created->activeContinuationCount() == 0U && created->activeAwaitableCount() == 0U);
        assert(created->stats().resume_queue_depth == 0U && created->failures().empty());
        assert(created->shutdown() && state.destroys == 3U);
        std::puts("RESUME_WRAP capacity=3 budget=2 cycles=17 calls=35 resumes=35 destroyed=35 backlog=0");
    }

    void testSyncAndContinuation()
    {
        Harness synchronous{false};
        auto sync_created = synchronous.create(limits(), {});
        assert(sync_created);
        auto sync_system = std::move(*sync_created);
        assert(sync_system.prepare());
        assert(dispatchRuntimeHook(sync_system, synchronous.hook) == 1U);
        assert(synchronous.backend_state.sync_calls == 1U);
        assert(sync_system.activeContinuationCount() == 0U);
        assert(sync_system.activeAwaitableCount() == 0U);
        assert(sync_system.shutdown());

        Harness eager{false};
        eager.backend_state.enable_step = true;
        eager.backend_state.eager_first = true;
        eager.backend_state.eager_resuspend = true;
        eager.backend_state.suspensions_after_first = 1U;
        auto eager_created = eager.create(limits(), {});
        assert(eager_created);
        auto eager_system = std::move(*eager_created);
        assert(eager_system.prepare());
        assert(dispatchRuntimeHook(eager_system, eager.hook) == 1U);
        assert(eager.backend_state.resume_calls == 0U);
        assert(eager_system.activeContinuationCount() == 1U);
        assert(dispatchRuntimeHook(eager_system, eager.hook) == 1U);
        assert(eager.backend_state.step_calls == 1U);
        assert(executeRuntimeStablePoint(eager_system));
        assert(eager.backend_state.resume_calls == 1U);
        assert(executeRuntimeStablePoint(eager_system));
        assert(eager.backend_state.resume_calls == 2U);
        assert(eager.backend_state.max_resume_depth == 1U);
        assert(eager.backend_state.continuation_destroys == 1U);
        assert(eager.backend_state.resume_thread == std::this_thread::get_id());
        assert(eager_system.activeContinuationCount() == 0U);
        assert(eager_system.activeAwaitableCount() == 0U);
        assert(eager_system.shutdown());

        Harness budgeted{false};
        budgeted.backend_state.enable_step = true;
        budgeted.backend_state.eager_first = true;
        budgeted.backend_state.eager_resuspend = true;
        budgeted.backend_state.suspensions_after_first = 2U;
        auto budgeted_created = budgeted.create(limits(1U, 8U, 8U, 8U, 1U), {});
        assert(budgeted_created);
        auto budgeted_system = std::move(*budgeted_created);
        assert(budgeted_system.prepare());
        assert(dispatchRuntimeHook(budgeted_system, budgeted.hook) == 1U);
        assert(executeRuntimeStablePoint(budgeted_system));
        assert(budgeted.backend_state.resume_calls == 1U);
        assert(budgeted_system.activeContinuationCount() == 1U);
        assert(executeRuntimeStablePoint(budgeted_system));
        assert(budgeted.backend_state.resume_calls == 2U);
        assert(executeRuntimeStablePoint(budgeted_system));
        assert(budgeted.backend_state.resume_calls == 3U);
        assert(budgeted_system.activeContinuationCount() == 0U);
        assert(budgeted_system.shutdown());

        Harness typed{false};
        typed.backend_state.enable_step = true;
        typed.backend_state.typed_result = true;
        auto typed_created = typed.create(limits(), {});
        assert(typed_created);
        auto typed_system = std::move(*typed_created);
        assert(typed_system.prepare());
        assert(dispatchRuntimeHook(typed_system, typed.hook) == 1U);
        ScriptOwnedResumeValue value;
        value.type = lux::rdesc::makeScriptValueType<std::int32_t>();
        assert(value.bytes.resize(sizeof(std::int32_t)));
        assert(typed.backend_state.completions.front().ready(std::move(value)));
        assert(executeRuntimeStablePoint(typed_system));
        assert(typed.backend_state.saw_typed_result);
        assert(typed_system.shutdown());

        Harness failed{false};
        failed.backend_state.enable_step = true;
        auto failed_created = failed.create(limits(), {});
        assert(failed_created);
        auto failed_system = std::move(*failed_created);
        assert(failed_system.prepare());
        assert(dispatchRuntimeHook(failed_system, failed.hook) == 1U);
        assert(failed.backend_state.completions.front().fail({93}));
        assert(executeRuntimeStablePoint(failed_system));
        assert(failed.backend_state.saw_failure);
        assert(failed_system.shutdown());
    }

    void testAsyncAbilityInvocation()
    {
        int constructions{}, destructions{};
        {
            TestProvider provider{constructions, destructions};
            provider.async_mode = EAsyncProviderMode::DELAYED;
            const std::array capabilities{
                publishScriptAbility(lux::script::bindScriptAbility<TestAbility>(provider))
            };
            Harness delayed{true};
            delayed.backend_state.enable_step = true;
            delayed.backend_state.enable_ability_async = true;
            auto created = delayed.create(limits(), capabilities);
            assert(created);
            auto system = std::move(*created);
            assert(system.prepare());
            assert(dispatchRuntimeHook(system, delayed.hook) == 1U);
            assert(provider.pending.has_value());
            assert(system.activeAwaitableCount() == 1U);
            assert(system.activeContinuationCount() == 1U);

            const auto completion = *provider.pending;
            std::optional<lux::cxx::expected<void, lux::script::EScriptAbilityCompletionError>> completed;
            std::thread worker([&]() noexcept { completed = completion.success(42U); });
            worker.join();
            assert(completed.has_value() && *completed);
            const auto duplicate = completion.success(43U);
            assert(!duplicate && duplicate.error() == lux::script::EScriptAbilityCompletionError::ALREADY_COMPLETED);
            assert(delayed.backend_state.resume_calls == 0U);
            assert(executeRuntimeStablePoint(system));
            assert(delayed.backend_state.resume_calls == 1U);
            assert(delayed.backend_state.ability_result == 42U);
            assert(delayed.backend_state.resume_thread == std::this_thread::get_id());
            assert(system.activeAwaitableCount() == 0U);
            assert(system.activeContinuationCount() == 0U);
            assert(system.shutdown());
        }

        {
            TestProvider provider{constructions, destructions};
            provider.async_mode = EAsyncProviderMode::EAGER_SUCCESS;
            const std::array capabilities{
                publishScriptAbility(lux::script::bindScriptAbility<TestAbility>(provider))
            };
            Harness eager{true};
            eager.backend_state.enable_step = true;
            eager.backend_state.enable_ability_async = true;
            auto created = eager.create(limits(), capabilities);
            assert(created);
            auto system = std::move(*created);
            assert(system.prepare());
            assert(dispatchRuntimeHook(system, eager.hook) == 1U);
            assert(provider.eager_completion.has_value() && *provider.eager_completion);
            assert(eager.backend_state.resume_calls == 0U);
            assert(executeRuntimeStablePoint(system));
            assert(eager.backend_state.resume_calls == 1U);
            assert(eager.backend_state.ability_result == 42U);
            assert(eager.backend_state.max_resume_depth == 1U);
            assert(system.shutdown());
        }

        {
            TestProvider provider{constructions, destructions};
            provider.async_mode = EAsyncProviderMode::EAGER_FAILURE;
            const std::array capabilities{
                publishScriptAbility(lux::script::bindScriptAbility<TestAbility>(provider))
            };
            Harness failed{true};
            failed.backend_state.enable_step = true;
            failed.backend_state.enable_ability_async = true;
            auto created = failed.create(limits(), capabilities);
            assert(created);
            auto system = std::move(*created);
            assert(system.prepare());
            assert(dispatchRuntimeHook(system, failed.hook) == 1U);
            assert(provider.eager_completion.has_value() && *provider.eager_completion);
            assert(executeRuntimeStablePoint(system));
            assert(failed.backend_state.saw_failure);
            assert(failed.backend_state.failure_status == 91);
            assert(system.shutdown());
        }

        {
            TestProvider provider{constructions, destructions};
            provider.async_mode = EAsyncProviderMode::REJECT;
            const std::array capabilities{
                publishScriptAbility(lux::script::bindScriptAbility<TestAbility>(provider))
            };
            Harness rejected{true};
            rejected.backend_state.enable_step = true;
            rejected.backend_state.enable_ability_async = true;
            auto created = rejected.create(limits(), capabilities);
            assert(created);
            auto system = std::move(*created);
            assert(system.prepare());
            assert(dispatchRuntimeHook(system, rejected.hook) == 1U);
            assert(system.activeAwaitableCount() == 0U);
            assert(system.activeContinuationCount() == 0U);
            assert(!system.failures().empty());
            assert(system.failures().front().error == EScriptSystemError::INVOCATION_FAILURE);
            assert(system.failures().front().status == 81);
            assert(system.shutdown());
        }

        {
            TestProvider provider{constructions, destructions};
            provider.async_mode = EAsyncProviderMode::DELAYED;
            const std::array capabilities{
                publishScriptAbility(lux::script::bindScriptAbility<TestAbility>(provider))
            };
            Harness late{true};
            late.backend_state.enable_step = true;
            late.backend_state.enable_ability_async = true;
            auto created = late.create(limits(), capabilities);
            assert(created);
            auto system = std::move(*created);
            assert(system.prepare());
            assert(dispatchRuntimeHook(system, late.hook) == 1U);
            assert(provider.pending.has_value());
            const auto completion = *provider.pending;
            assert(system.shutdown());
            const auto completed = completion.success(44U);
            assert(!completed && completed.error() == lux::script::EScriptAbilityCompletionError::STOPPING);
            assert(late.backend_state.resume_calls == 0U);
        }

        {
            Harness concurrent{false, 2U, false, true};
            concurrent.backend_state.enable_step = true;
            auto created = concurrent.create(limits(2U, 2U, 2U, 2U, 2U), {});
            assert(created);
            auto system = std::move(*created);
            assert(system.prepare());
            assert(dispatchRuntimeHook(system, concurrent.hook) == 1U);
            assert(dispatchRuntimeHook(system, concurrent.hook_third) == 1U);
            assert(concurrent.backend_state.completions.size() == 2U);
            std::array<std::optional<lux::cxx::expected<void, EScriptAwaitableCompletionError>>, 2U> results;
            std::array<std::thread, 2U> producers{
                std::thread{[&]() noexcept { results[0] = concurrent.backend_state.completions[0].ready(); }},
                std::thread{[&]() noexcept { results[1] = concurrent.backend_state.completions[1].ready(); }}
            };
            for (auto& producer : producers)
                producer.join();
            assert(results[0].has_value() && *results[0]);
            assert(results[1].has_value() && *results[1]);
            assert(system.stats().external_completion_queue_depth == 2U);
            assert(executeRuntimeStablePoint(system));
            assert(concurrent.backend_state.resume_calls == 2U);
            assert(system.shutdown());
        }
        assert(constructions == destructions);
    }

    void testCapacityAndCancellation()
    {
        Harness invalid_quota{false};
        const auto invalid_quota_created = invalid_quota.create(limits(1U, 1U, 1U, 1U, 1U, 2U), {});
        assert(!invalid_quota_created && invalid_quota_created.error() == EScriptSystemError::INVALID_INPUT);

        Harness invalid_next_step{false};
        auto invalid_next_step_limits = limits();
        invalid_next_step_limits.next_step_wait_capacity = 0U;
        const auto invalid_next_step_created = invalid_next_step.create(invalid_next_step_limits, {});
        assert(!invalid_next_step_created && invalid_next_step_created.error() == EScriptSystemError::INVALID_INPUT);

        Harness invalid_simulation_delay{false};
        auto invalid_simulation_delay_limits = limits();
        invalid_simulation_delay_limits.simulation_delay_capacity = 0U;
        const auto invalid_simulation_delay_created = invalid_simulation_delay.create(
            invalid_simulation_delay_limits,
            {}
        );
        assert(
            !invalid_simulation_delay_created &&
            invalid_simulation_delay_created.error() == EScriptSystemError::INVALID_INPUT
        );

        Harness invalid_external_completion{false};
        auto invalid_external_completion_limits = limits();
        invalid_external_completion_limits.external_completion_capacity = 0U;
        const auto invalid_external_completion_created = invalid_external_completion.create(
            invalid_external_completion_limits,
            {}
        );
        assert(
            !invalid_external_completion_created &&
            invalid_external_completion_created.error() == EScriptSystemError::INVALID_INPUT
        );

        Harness per_instance{false, 2U, false, true};
        per_instance.backend_state.enable_step = true;
        auto per_instance_created = per_instance.create(limits(2U, 3U, 3U, 3U, 3U, 1U), {});
        assert(per_instance_created);
        auto per_instance_system = std::move(*per_instance_created);
        assert(per_instance_system.prepare());
        assert(dispatchRuntimeHook(per_instance_system, per_instance.hook) == 1U);
        assert(dispatchRuntimeHook(per_instance_system, per_instance.hook_third) == 1U);
        assert(per_instance_system.activeContinuationCount() == 2U);
        assert(dispatchRuntimeHook(per_instance_system, per_instance.hook_second) == 1U);
        assert(per_instance_system.activeContinuationCount() == 2U);
        assert(std::any_of(
            per_instance_system.failures().begin(),
            per_instance_system.failures().end(),
            [](const ScriptSystemFailure& failure) noexcept {
                return failure.error == EScriptSystemError::INSTANCE_CONTINUATION_CAPACITY_EXCEEDED;
            }
        ));
        assert(per_instance.backend_state.completions.size() == 3U);
        assert(executeRuntimeStablePoint(per_instance_system));
        assert(per_instance_system.activeContinuationCount() == 1U);
        assert(per_instance.backend_state.completions[1].ready());
        assert(executeRuntimeStablePoint(per_instance_system));
        assert(per_instance_system.activeContinuationCount() == 0U);
        assert(per_instance_system.shutdown());

        Harness quota_return{false, 2U, false, true};
        quota_return.backend_state.enable_step = true;
        auto quota_return_created = quota_return.create(limits(2U, 2U, 2U, 2U, 2U, 1U), {});
        assert(quota_return_created);
        auto quota_return_system = std::move(*quota_return_created);
        assert(quota_return_system.prepare());
        assert(dispatchRuntimeHook(quota_return_system, quota_return.hook) == 1U);
        assert(quota_return.backend_state.completions.front().ready());
        assert(executeRuntimeStablePoint(quota_return_system));
        assert(quota_return_system.activeContinuationCount() == 0U);
        assert(dispatchRuntimeHook(quota_return_system, quota_return.hook_second) == 1U);
        assert(quota_return_system.activeContinuationCount() == 1U);
        assert(quota_return.backend_state.completions[1].ready());
        assert(executeRuntimeStablePoint(quota_return_system));
        assert(quota_return_system.activeContinuationCount() == 0U);
        assert(quota_return_system.shutdown());

        Harness limited{false, 2U};
        limited.backend_state.enable_step = true;
        auto limited_created = limited.create(limits(2U, 1U, 2U, 1U, 1U), {});
        assert(limited_created);
        auto limited_system = std::move(*limited_created);
        assert(limited_system.prepare());
        assert(dispatchRuntimeHook(limited_system, limited.hook) == 1U);
        assert(limited_system.activeContinuationCount() == 1U);
        assert(!limited_system.failures().empty());
        assert(limited_system.failures().front().error == EScriptSystemError::CONTINUATION_CAPACITY_EXCEEDED ||
               limited_system.failures().front().error == EScriptSystemError::INVOCATION_FAILURE);
        assert(limited_system.shutdown());

        Harness awaitable_limited{false, 2U};
        awaitable_limited.backend_state.enable_step = true;
        auto awaitable_created = awaitable_limited.create(limits(2U, 2U, 1U, 2U, 2U), {});
        assert(awaitable_created);
        auto awaitable_system = std::move(*awaitable_created);
        assert(awaitable_system.prepare());
        assert(dispatchRuntimeHook(awaitable_system, awaitable_limited.hook) == 1U);
        assert(awaitable_system.activeAwaitableCount() == 1U);
        assert(!awaitable_system.failures().empty());
        assert(awaitable_system.shutdown());

        Harness queue_limited{false, 2U};
        queue_limited.backend_state.enable_step = true;
        auto queue_created = queue_limited.create(limits(2U, 2U, 2U, 2U, 2U, 0U, 1U), {});
        assert(queue_created);
        auto queue_system = std::move(*queue_created);
        assert(queue_system.prepare());
        assert(dispatchRuntimeHook(queue_system, queue_limited.hook) == 1U);
        assert(queue_limited.backend_state.completions.size() == 2U);
        assert(queue_limited.backend_state.completions[0].ready());
        const auto full = queue_limited.backend_state.completions[1].ready();
        assert(!full && full.error() == EScriptAwaitableCompletionError::RESUME_QUEUE_FULL);
        assert(executeRuntimeStablePoint(queue_system));
        assert(queue_limited.backend_state.completions[1].ready());
        assert(executeRuntimeStablePoint(queue_system));
        assert(queue_system.activeContinuationCount() == 0U);
        assert(queue_system.shutdown());

        Harness stale{false, 1U, true};
        stale.backend_state.enable_step = true;
        auto stale_created = stale.create(limits(), {});
        assert(stale_created);
        auto stale_system = std::move(*stale_created);
        assert(stale_system.prepare());
        assert(dispatchRuntimeHook(stale_system, stale.hook) == 1U);
        assert(stale.backend_state.completions.size() == 1U);
        const auto completion = stale.backend_state.completions.front();
        stale.registry.destroy(stale.entity);
        const auto detached = executeRuntimeStablePoint(stale_system);
        assert(detached);
        const auto stale_completion = completion.ready();
        assert(!stale_completion && stale_completion.error() == EScriptAwaitableCompletionError::INVALID_ID);
        assert(stale_system.shutdown());
        assert(stale.backend_state.continuation_destroys == 1U);

        for (std::size_t iteration{}; iteration < 50U; ++iteration)
        {
            Harness race{false, 1U, true};
            race.backend_state.enable_step = true;
            auto race_created = race.create(limits(), {});
            assert(race_created);
            auto race_system = std::move(*race_created);
            assert(race_system.prepare());
            assert(dispatchRuntimeHook(race_system, race.hook) == 1U);
            const auto race_completion = race.backend_state.completions.front();
            std::optional<lux::cxx::expected<void, EScriptAwaitableCompletionError>> completion_result;
            std::thread completing([&]() noexcept { completion_result = race_completion.ready(); });
            race.registry.destroy(race.entity);
            static_cast<void>(executeRuntimeStablePoint(race_system));
            completing.join();
            assert(completion_result.has_value());
            assert(*completion_result || completion_result->error() == EScriptAwaitableCompletionError::INVALID_ID);
            assert(race.backend_state.resume_calls == 0U);
            assert(race.backend_state.continuation_destroys == 1U);
            assert(race_system.shutdown());
        }
    }
} // namespace

template <std::size_t Size, std::size_t Alignment = 1U>
void testExternalAdmission()
{
    using Payload = lux::simulation::script::test::ExternalPayload<Size, Alignment>;
    constexpr bool Supported = sizeof(Payload) <= 32U && alignof(Payload) <= alignof(std::max_align_t);
    Harness harness{false};
    harness.backend_state.enable_step = true;
    harness.backend_state.expected_payload_size = sizeof(Payload);
    harness.backend_state.custom_step = [](BackendState& state, ScriptStepContext& step) noexcept {
        return invokeScriptAbilityAsync<Payload>(step,
            [&state](lux::script::ScriptAbilityCompletion<Payload> completion) noexcept {
                ++state.provider_starts;
                static_cast<void>(completion.success(Payload{}));
                return lux::script::ScriptAbilityStartResult{};
            });
    };
    auto runtime_limits = limits();
    runtime_limits.max_resume_payload_bytes = 128U;
    auto created = harness.create(runtime_limits, {});
    assert(created);
    auto& system = *created;
    assert(system.prepare());
    assert(dispatchRuntimeHook(system, harness.hook) == 1U);
    assert(harness.backend_state.provider_starts == (Supported ? 1U : 0U));
    assert(system.activeAwaitableCount() == (Supported ? 1U : 0U));
    assert(system.activeContinuationCount() == (Supported ? 1U : 0U));
    static_cast<void>(executeRuntimeStablePoint(system));
    assert(harness.backend_state.resume_calls == (Supported ? 1U : 0U));
    assert(system.activeAwaitableCount() == 0U && system.activeContinuationCount() == 0U);
    assert(system.shutdown());
}

int main()
{
    testTimerSourceCancellation();
#if defined(LUX_SCRIPT_SOURCE_PROTOCOL_CLOCK)
    testTimerDeadlineOrderAndBackpressure();
#endif
    {
        Harness harness{false, 1U, true};
        harness.backend_state.enable_step = true;
        auto created = harness.create(limits(), {});
        assert(created && created->prepare());
        struct ActiveRetirement final { Harness* harness; ScriptSystem* system; } active{&harness, &*created};
        harness.backend_state.resume_probe_context = &active;
        harness.backend_state.resume_probe = [](void* context) noexcept {
            auto& active = *static_cast<ActiveRetirement*>(context);
            active.harness->registry.destroy(active.harness->entity);
            const auto lifecycle = active.system->processLifecycle();
            assert(!lifecycle && lifecycle.error() == EScriptSystemError::ENDPOINT_BUSY);
            assert(active.harness->backend_state.continuation_destroys == 0U);
            assert(active.harness->backend_state.destroys == 0U);
        };
        assert(dispatchRuntimeHook(*created, harness.hook) == 1U);
        assert(harness.backend_state.completions.front().ready());
        assert(executeRuntimeStablePoint(*created));
        assert(harness.backend_state.resume_calls == 1U);
        static_cast<void>(created->processLifecycle());
        assert(harness.backend_state.continuation_destroys == 1U);
        assert(harness.backend_state.destroys == 1U);
        assert(created->shutdown());
    }
    testExternalAdmission<32U>();
    testExternalAdmission<33U>();
    testExternalAdmission<64U>();
    testExternalAdmission<32U, 32U>();
    {
        Harness harness{false, 1U, false, false, true};
        harness.backend_state.enable_step = true;
        harness.backend_state.expected_payload_size = sizeof(LargePayload);
        harness.backend_state.custom_step = [](BackendState& state, ScriptStepContext& step) noexcept {
            const auto waiting = step.event_waits.wait(state.event);
            assert(waiting);
            return ScriptStepResult::suspended(*waiting);
        };
        auto created = harness.create(limits(), {});
        assert(created && created->prepare());
        assert(dispatchRuntimeHook(*created, harness.hook) == 1U);
        {
            auto writer = harness.large_event.begin(0U);
            assert(writer.record(LargePayload{}));
        }
        assert(deliverRuntimeEvent(*created, harness.large_bridge) == 1U);
        assert(harness.backend_state.resume_calls == 0U);
        assert(executeRuntimeStablePoint(*created));
        assert(harness.backend_state.resume_calls == 1U);
        assert(created->shutdown());
    }
    testCapabilities();
    testSingleFlightIsolation();
    testSharedHookMethod();
    testHookRatios();
    testNonPowerOfTwoResumeWrap();
    testSyncAndContinuation();
    testAsyncAbilityInvocation();
    testCapacityAndCancellation();
    return 0;
}

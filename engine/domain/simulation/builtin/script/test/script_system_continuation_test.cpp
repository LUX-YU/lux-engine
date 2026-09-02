#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include "TestAbility.hpp"
#include "TestAbility.ability.generated.hpp"

#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/scripting/ScriptAbilityInvocation.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    inline constexpr lux::system::SystemInstanceId kSystem{0x5101U};
    inline constexpr HookPointId kHook{0x5102U};
    inline constexpr HookPointId kHookSecond{0x5104U};
    inline constexpr HookPointId kHookThird{0x5105U};
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

    [[nodiscard]] lux::world::WorldObjectId objectId()
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[0] = 0x52U;
        return lux::world::WorldObjectId{uuids::uuid{bytes}};
    }

    [[nodiscard]] SimulationDescription makeSimulation()
    {
        constexpr std::array hooks{
            makeHookPointSpec<void()>(kHook, "tick"),
            makeHookPointSpec<void()>(kHookSecond, "tick-second"),
            makeHookPointSpec<void()>(kHookThird, "tick-third")
        };
        const SimulationSystemDescription system{
            .type = {.canonical_name = "lux.test.script-continuation", .version = 1U},
            .hooks = hooks};
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
        ++state.capability_bind_scans;
        if (!create.capabilities.empty())
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

    EScriptBackendResult prepareMethod(void*,
                                       ScriptBackendInstance instance,
                                       const lux::rdesc::ScriptFunction&,
                                       lux::script::BoundScriptCall& output) noexcept
    {
        auto* prepared = new (std::nothrow) PreparedSync();
        if (prepared == nullptr)
            return EScriptBackendResult::ALLOCATION_FAILURE;
        prepared->instance = static_cast<BackendInstance*>(instance.value);
        output = {&invokeSync, prepared};
        return EScriptBackendResult::SUCCESS;
    }

    void releaseMethod(void*, ScriptBackendInstance, lux::script::BoundScriptCall call) noexcept
    {
        delete static_cast<PreparedSync*>(call.context);
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
        delete continuation;
    }

    ScriptStepResult resumeContinuation(void* value,
                                        ScriptStepContext& context,
                                        const ScriptResumePacket& packet) noexcept
    {
        auto& continuation = *static_cast<ContinuationState*>(value);
        auto& owner = *continuation.owner;
        if (packet.state == EScriptAwaitableState::FAILED)
        {
            assert(packet.error.valid());
            owner.saw_failure = true;
            owner.failure_status = packet.error.status;
        }
        else
        {
            assert(packet.state == EScriptAwaitableState::READY);
            if (owner.enable_ability_async)
            {
                assert(packet.value != nullptr && packet.value->type.has_value());
                assert(packet.value->type->type_id == lux::semantic::typeId("lux.u64"));
                assert(packet.value->bytes.size() == sizeof(std::uint64_t));
                std::memcpy(
                    std::addressof(owner.ability_result),
                    packet.value->bytes.data(),
                    sizeof(owner.ability_result)
                );
            }
            else if (owner.typed_result)
            {
                assert(packet.value != nullptr && packet.value->type.has_value());
                assert(packet.value->type->type_id == lux::semantic::typeId("lux.i32"));
                assert(packet.value->bytes.size() == sizeof(std::int32_t));
                owner.saw_typed_result = true;
            }
        }
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
        if (state.enable_ability_async)
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

    EScriptBackendResult prepareStepMethod(void* context,
                                           ScriptBackendInstance,
                                           const lux::rdesc::ScriptFunction&,
                                           BoundScriptStepCall& output) noexcept
    {
        auto& state = *static_cast<BackendState*>(context);
        if (state.enable_step)
            output = {&state, &invokeStep};
        return EScriptBackendResult::SUCCESS;
    }

    void releaseStepMethod(void*, ScriptBackendInstance, BoundScriptStepCall) noexcept {}

    struct Harness final
    {
        explicit Harness(
            bool require_capability,
            std::size_t mount_count = 1U,
            bool entity_scope = false,
            bool quota_layout = false
        )
            : simulation(makeSimulation()), artifact(makeArtifact(require_capability)), asset(assetId()),
              object(objectId()), uses_entity_scope(entity_scope)
        {
            if (uses_entity_scope)
                entity = registry.create();
            ScriptSystemDescriptionBuilder builder;
            if (quota_layout)
            {
                assert(mount_count == 2U && !uses_entity_scope);
                assert(builder.addMount({
                    ScriptMountId{1U},
                    asset,
                    SimulationScriptMount{},
                    true,
                    {
                        {kSymbol, HookScriptTarget{kSystem, kHook}},
                        {kSymbolSecond, HookScriptTarget{kSystem, kHookSecond}}
                    }
                }));
                assert(builder.addMount({
                    ScriptMountId{2U},
                    asset,
                    SimulationScriptMount{},
                    true,
                    {{kSymbolThird, HookScriptTarget{kSystem, kHookThird}}}
                }));
            }
            else
            {
                for (std::size_t index{}; index < mount_count; ++index)
                {
                    assert(builder.addMount({ScriptMountId{index + 1U},
                                             asset,
                                             uses_entity_scope ? ScriptMountScope{EntityScriptMount{object}}
                                                               : ScriptMountScope{SimulationScriptMount{}},
                                             true,
                                             {{kSymbol, HookScriptTarget{kSystem, kHook}}}}));
                }
            }
            auto built = std::move(builder).build(simulation);
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
                       &destroyInstance,
                       &prepareStepMethod,
                       &releaseStepMethod};
        }

        [[nodiscard]] lux::cxx::expected<ScriptSystem, EScriptSystemError> create(
            ScriptRuntimeLimits limits,
            std::span<const ScriptApiCapabilityPublication> capabilities,
            bool include_endpoint = true) noexcept
        {
            return ScriptSystem::create(
                simulation,
                description,
                registry,
                limits,
                {this, &resolveArtifact},
                uses_entity_scope ? WorldObjectResolver{this, &resolveWorld} : WorldObjectResolver{},
                capabilities,
                std::span{&backend, 1U},
                include_endpoint ? std::span<const ScriptHookEndpointDescriptor>{endpoints}
                                 : std::span<const ScriptHookEndpointDescriptor>{},
                {});
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

        static bool resolveWorld(void* context,
                                 const lux::world::WorldObjectId& requested,
                                 ecs::Entity& output) noexcept
        {
            auto& self = *static_cast<Harness*>(context);
            if (requested != self.object)
                return false;
            output = self.entity;
            return true;
        }

        SimulationDescription simulation;
        ScriptSystemDescription description;
        lux::script::ScriptArtifact artifact;
        lux::asset::AssetId asset;
        lux::world::WorldObjectId object;
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
    };

    [[nodiscard]] ScriptRuntimeLimits limits(std::size_t instances = 1U,
                                             std::size_t continuations = 8U,
                                             std::size_t awaitables = 8U,
                                             std::size_t resumes = 8U,
                                             std::size_t budget = 8U,
                                             std::size_t continuations_per_instance = 0U) noexcept
    {
        const std::size_t per_instance = continuations_per_instance == 0U
            ? continuations
            : continuations_per_instance;
        return {16U, instances, continuations, per_instance, awaitables, resumes, 64U, budget};
    }

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
            assert(available.hook.dispatch() == 1U);
            assert(available.hook.dispatch() == 1U);
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
        assert(endpoint_created);
        auto endpoint_system = std::move(*endpoint_created);
        const auto endpoint_prepared = endpoint_system.prepare();
        assert(!endpoint_prepared && endpoint_prepared.error() == EScriptSystemError::SCRIPT_ENDPOINT_NOT_FOUND);
        assert(endpoint_missing.backend_state.creates == 0U);
        assert(endpoint_system.shutdown());
    }

    void testSyncAndContinuation()
    {
        Harness synchronous{false};
        auto sync_created = synchronous.create(limits(), {});
        assert(sync_created);
        auto sync_system = std::move(*sync_created);
        assert(sync_system.prepare());
        assert(synchronous.hook.dispatch() == 1U);
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
        assert(eager.hook.dispatch() == 1U);
        assert(eager.backend_state.resume_calls == 0U);
        assert(eager_system.activeContinuationCount() == 1U);
        assert(eager.hook.dispatch() == 1U);
        assert(eager.backend_state.step_calls == 1U);
        assert(eager_system.executeStablePoint());
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
        assert(budgeted.hook.dispatch() == 1U);
        assert(budgeted_system.executeStablePoint());
        assert(budgeted.backend_state.resume_calls == 1U);
        assert(budgeted_system.activeContinuationCount() == 1U);
        assert(budgeted_system.executeStablePoint());
        assert(budgeted.backend_state.resume_calls == 2U);
        assert(budgeted_system.executeStablePoint());
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
        assert(typed.hook.dispatch() == 1U);
        ScriptOwnedResumeValue value;
        value.type = lux::rdesc::makeScriptValueType<std::int32_t>();
        value.bytes.resize(sizeof(std::int32_t));
        assert(typed.backend_state.completions.front().ready(std::move(value)));
        assert(typed_system.executeStablePoint());
        assert(typed.backend_state.saw_typed_result);
        assert(typed_system.shutdown());

        Harness failed{false};
        failed.backend_state.enable_step = true;
        auto failed_created = failed.create(limits(), {});
        assert(failed_created);
        auto failed_system = std::move(*failed_created);
        assert(failed_system.prepare());
        assert(failed.hook.dispatch() == 1U);
        assert(failed.backend_state.completions.front().fail({93}));
        assert(failed_system.executeStablePoint());
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
            assert(delayed.hook.dispatch() == 1U);
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
            assert(system.executeStablePoint());
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
            assert(eager.hook.dispatch() == 1U);
            assert(provider.eager_completion.has_value() && *provider.eager_completion);
            assert(eager.backend_state.resume_calls == 0U);
            assert(system.executeStablePoint());
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
            assert(failed.hook.dispatch() == 1U);
            assert(provider.eager_completion.has_value() && *provider.eager_completion);
            assert(system.executeStablePoint());
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
            assert(rejected.hook.dispatch() == 1U);
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
            assert(late.hook.dispatch() == 1U);
            assert(provider.pending.has_value());
            const auto completion = *provider.pending;
            assert(system.shutdown());
            const auto completed = completion.success(44U);
            assert(!completed && completed.error() == lux::script::EScriptAbilityCompletionError::STOPPING);
            assert(late.backend_state.resume_calls == 0U);
        }
        assert(constructions == destructions);
    }

    void testCapacityAndCancellation()
    {
        Harness invalid_quota{false};
        const auto invalid_quota_created = invalid_quota.create(limits(1U, 1U, 1U, 1U, 1U, 2U), {});
        assert(!invalid_quota_created && invalid_quota_created.error() == EScriptSystemError::INVALID_INPUT);

        Harness per_instance{false, 2U, false, true};
        per_instance.backend_state.enable_step = true;
        auto per_instance_created = per_instance.create(limits(2U, 3U, 3U, 3U, 3U, 1U), {});
        assert(per_instance_created);
        auto per_instance_system = std::move(*per_instance_created);
        assert(per_instance_system.prepare());
        assert(per_instance.hook.dispatch() == 1U);
        assert(per_instance.hook_third.dispatch() == 1U);
        assert(per_instance_system.activeContinuationCount() == 2U);
        assert(per_instance.hook_second.dispatch() == 1U);
        assert(per_instance_system.activeContinuationCount() == 2U);
        assert(std::any_of(
            per_instance_system.failures().begin(),
            per_instance_system.failures().end(),
            [](const ScriptSystemFailure& failure) noexcept {
                return failure.error == EScriptSystemError::INSTANCE_CONTINUATION_CAPACITY_EXCEEDED;
            }
        ));
        assert(per_instance.backend_state.completions.size() == 3U);
        assert(per_instance_system.executeStablePoint());
        assert(per_instance_system.activeContinuationCount() == 1U);
        assert(per_instance.backend_state.completions[1].ready());
        assert(per_instance_system.executeStablePoint());
        assert(per_instance_system.activeContinuationCount() == 0U);
        assert(per_instance_system.shutdown());

        Harness quota_return{false, 2U, false, true};
        quota_return.backend_state.enable_step = true;
        auto quota_return_created = quota_return.create(limits(2U, 2U, 2U, 2U, 2U, 1U), {});
        assert(quota_return_created);
        auto quota_return_system = std::move(*quota_return_created);
        assert(quota_return_system.prepare());
        assert(quota_return.hook.dispatch() == 1U);
        assert(quota_return.backend_state.completions.front().ready());
        assert(quota_return_system.executeStablePoint());
        assert(quota_return_system.activeContinuationCount() == 0U);
        assert(quota_return.hook_second.dispatch() == 1U);
        assert(quota_return_system.activeContinuationCount() == 1U);
        assert(quota_return.backend_state.completions[1].ready());
        assert(quota_return_system.executeStablePoint());
        assert(quota_return_system.activeContinuationCount() == 0U);
        assert(quota_return_system.shutdown());

        Harness limited{false, 2U};
        limited.backend_state.enable_step = true;
        auto limited_created = limited.create(limits(2U, 1U, 2U, 1U, 1U), {});
        assert(limited_created);
        auto limited_system = std::move(*limited_created);
        assert(limited_system.prepare());
        assert(limited.hook.dispatch() == 1U);
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
        assert(awaitable_limited.hook.dispatch() == 1U);
        assert(awaitable_system.activeAwaitableCount() == 1U);
        assert(!awaitable_system.failures().empty());
        assert(awaitable_system.shutdown());

        Harness queue_limited{false, 2U};
        queue_limited.backend_state.enable_step = true;
        auto queue_created = queue_limited.create(limits(2U, 2U, 2U, 1U, 2U), {});
        assert(queue_created);
        auto queue_system = std::move(*queue_created);
        assert(queue_system.prepare());
        assert(queue_limited.hook.dispatch() == 1U);
        assert(queue_limited.backend_state.completions.size() == 2U);
        assert(queue_limited.backend_state.completions[0].ready());
        const auto full = queue_limited.backend_state.completions[1].ready();
        assert(!full && full.error() == EScriptAwaitableCompletionError::RESUME_QUEUE_FULL);
        assert(queue_system.executeStablePoint());
        assert(queue_limited.backend_state.completions[1].ready());
        assert(queue_system.executeStablePoint());
        assert(queue_system.activeContinuationCount() == 0U);
        assert(queue_system.shutdown());

        Harness stale{false, 1U, true};
        stale.backend_state.enable_step = true;
        auto stale_created = stale.create(limits(), {});
        assert(stale_created);
        auto stale_system = std::move(*stale_created);
        assert(stale_system.prepare());
        assert(stale.hook.dispatch() == 1U);
        assert(stale.backend_state.completions.size() == 1U);
        const auto completion = stale.backend_state.completions.front();
        stale.registry.destroy(stale.entity);
        const auto detached = stale_system.executeStablePoint();
        assert(!detached && detached.error() == EScriptSystemError::WORLD_OBJECT_NOT_RESOLVED);
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
            assert(race.hook.dispatch() == 1U);
            const auto race_completion = race.backend_state.completions.front();
            std::optional<lux::cxx::expected<void, EScriptAwaitableCompletionError>> completion_result;
            std::thread completing([&]() noexcept { completion_result = race_completion.ready(); });
            race.registry.destroy(race.entity);
            static_cast<void>(race_system.executeStablePoint());
            completing.join();
            assert(completion_result.has_value());
            assert(*completion_result || completion_result->error() == EScriptAwaitableCompletionError::INVALID_ID);
            assert(race.backend_state.resume_calls == 0U);
            assert(race.backend_state.continuation_destroys == 1U);
            assert(race_system.shutdown());
        }
    }
} // namespace

int main()
{
    testCapabilities();
    testSyncAndContinuation();
    testAsyncAbilityInvocation();
    testCapacityAndCancellation();
    return 0;
}

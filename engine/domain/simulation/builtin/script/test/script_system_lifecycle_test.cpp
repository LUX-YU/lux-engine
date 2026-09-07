#include "../../../system/test/HookInvocationTestAccess.hpp"
using lux::simulation::test::dispatchHookForTest;
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/scripting/ScriptLifecycle.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace
{
    using namespace lux::simulation;
    using namespace lux::simulation::ecs;
    using namespace lux::simulation::script;

    inline constexpr lux::system::SystemInstanceId kSystem{0x7501U};
    inline constexpr HookPointId kHook{0x7502U};
    inline constexpr lux::script::ScriptSymbolId kBegin{0x7503U};
    inline constexpr lux::script::ScriptSymbolId kTick{0x7504U};
    inline constexpr lux::script::ScriptSymbolId kEnd{0x7505U};

    [[nodiscard]] lux::asset::AssetId assetId() noexcept
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[0] = 0x75U;
        return lux::asset::AssetId{bytes};
    }



    [[nodiscard]] SimulationDescription makeSimulation()
    {
        constexpr std::array hooks{makeHookPointSpec<void()>(kHook, "tick")};
        const SimulationSystemDescription system{
            .type = {.canonical_name = "lux.test.script-lifecycle", .version = 1U},
            .hooks = hooks
        };
        SimulationDescriptionBuilder builder;
        assert(builder.addSystem(kSystem, "script-lifecycle", system));
        auto result = std::move(builder).build();
        assert(result);
        return std::move(*result);
    }

    [[nodiscard]] lux::script::ScriptArtifact makeArtifact(
        bool begin,
        bool end,
        bool invalid_begin = false,
        bool invalid_end = false
    )
    {
        lux::rdesc::Script description;
        description.module_name = "lux.test.script-lifecycle.fixture";
        if (begin)
        {
            auto args = invalid_begin
                ? std::vector{lux::rdesc::makeScriptValueType<std::uint32_t>()}
                : std::vector<lux::rdesc::ScriptValueType>{};
            description.exports.push_back({"initialize", kBegin, std::move(args), {}});
            description.lifecycle.begin_play = kBegin;
        }
        description.exports.push_back({"update", kTick, {}, {}});
        if (end)
        {
            auto argument = invalid_end
                ? lux::rdesc::makeScriptValueType<std::uint32_t>()
                : lux::rdesc::makeScriptValueType<EScriptEndPlayReason>();
            description.exports.push_back({"retire", kEnd, {std::move(argument)}, {}});
            description.lifecycle.end_play = kEnd;
        }
        description.body = lux::rdesc::CppStaticScript{"lifecycle-fixture"};
        auto result = lux::script::ScriptArtifact::create(std::move(description), {});
        assert(result);
        return std::move(*result);
    }

    struct BackendState;

    struct Instance final
    {
        BackendState* owner{};
        std::size_t serial{};
        std::uint32_t value{};
    };

    struct PreparedCall final
    {
        Instance* instance{};
        lux::script::ScriptSymbolId symbol{};
    };

    struct Continuation final
    {
        BackendState* owner{};
        Instance* instance{};
    };

    struct BackendState final
    {
        std::size_t creates{};
        std::size_t prepares{};
        std::size_t releases{};
        std::size_t begins{};
        std::size_t ends{};
        std::size_t destroys{};
        std::size_t first_begin_create_count{};
        std::size_t resumes{};
        std::size_t tick_calls{};
        std::size_t continuation_destroys{};
        std::size_t fail_begin_serial{};
        std::size_t fail_tick_serial{};
        std::size_t fail_prepare_ordinal{};
        bool fail_create{};
        bool fail_end{};
        bool async_tick{};
        HookPoint<void()>* hook{};
        std::vector<std::uint32_t> end_values;
        std::vector<EScriptEndPlayReason> end_reasons;
        std::vector<std::size_t> end_normal_dispatches;
        std::vector<ScriptAwaitableCompletion> completions;
        std::vector<ScriptBehavior*> hosts;
    };

    int invokePrepared(lux_script_call_frame* frame) noexcept
    {
        auto& call = *static_cast<PreparedCall*>(frame->user_context);
        auto& instance = *call.instance;
        auto& state = *instance.owner;
        if (call.symbol == kBegin)
        {
            if (state.first_begin_create_count == 0U)
                state.first_begin_create_count = state.creates;
            if (state.fail_begin_serial == instance.serial)
                return 31;
            instance.value = static_cast<std::uint32_t>(10U + instance.serial);
            ++state.begins;
            return 0;
        }
        if (call.symbol == kTick)
        {
            ++state.tick_calls;
            if (state.fail_tick_serial == instance.serial)
                return 32;
            ++instance.value;
            return 0;
        }
        if (call.symbol == kEnd)
        {
            assert(frame->arg_count == 1U && frame->args != nullptr);
            assert(frame->args[0].kind == LUX_SCRIPT_VK_UINT32);
            const auto reason = *static_cast<const EScriptEndPlayReason*>(frame->args[0].data);
            ++state.ends;
            state.end_values.push_back(instance.value);
            state.end_reasons.push_back(reason);
            const auto calls_before = state.tick_calls;
            if (state.hook)
                static_cast<void>(dispatchHookForTest(*state.hook));
            state.end_normal_dispatches.push_back(state.tick_calls - calls_before);
            return state.fail_end ? 41 : 0;
        }
        return 42;
    }

    EScriptBackendResult createInstance(
        void* context,
        const ScriptInstanceCreateContext& create_context,
        const lux::script::ScriptArtifact&,
        ScriptBackendInstance& output
    ) noexcept
    {
        auto& state = *static_cast<BackendState*>(context);
        if (state.fail_create)
            return EScriptBackendResult::CONSTRUCTION_FAILURE;
        auto* instance = new (std::nothrow) Instance{&state, state.creates + 1U, 0U};
        if (instance == nullptr)
            return EScriptBackendResult::ALLOCATION_FAILURE;
        ++state.creates;
        state.hosts.push_back(create_context.behavior);
        output.value = instance;
        return EScriptBackendResult::SUCCESS;
    }

    ScriptStepResult invokeAsync(
        void* context,
        lux_script_call_frame&,
        ScriptStepContext& step,
        ScriptBackendContinuation& output
    ) noexcept;

    EScriptBackendResult prepareMethod(
        void* context,
        ScriptBackendInstance instance,
        const lux::rdesc::ScriptFunction& function,
        ScriptBackendPreparedMethod& output
    ) noexcept
    {
        auto& state = *static_cast<BackendState*>(context);
        if (state.fail_prepare_ordinal == state.prepares + 1U)
            return EScriptBackendResult::UNSUPPORTED_SIGNATURE;
        auto* call = new (std::nothrow) PreparedCall{
            static_cast<Instance*>(instance.value),
            function.symbol_id
        };
        if (call == nullptr)
            return EScriptBackendResult::ALLOCATION_FAILURE;
        ++state.prepares;
        output = {
            call,
            lux::script::BoundScriptCall{&invokePrepared, call},
            state.async_tick && function.symbol_id == kTick
                ? BoundScriptStepCall{call, &invokeAsync}
                : BoundScriptStepCall{}
        };
        return EScriptBackendResult::SUCCESS;
    }

    void releaseMethod(void* context, ScriptBackendInstance, ScriptBackendPreparedMethod method) noexcept
    {
        ++static_cast<BackendState*>(context)->releases;
        delete static_cast<PreparedCall*>(method.token);
    }

    void destroyInstance(void* context, ScriptBackendInstance instance) noexcept
    {
        ++static_cast<BackendState*>(context)->destroys;
        delete static_cast<Instance*>(instance.value);
    }

    ScriptStepResult resumeAsync(void* opaque, ScriptStepContext&, const ScriptResumePacket&) noexcept
    {
        auto& continuation = *static_cast<Continuation*>(opaque);
        ++continuation.owner->resumes;
        ++continuation.instance->value;
        return ScriptStepResult::completed();
    }

    void destroyContinuation(void* opaque) noexcept
    {
        auto* continuation = static_cast<Continuation*>(opaque);
        ++continuation->owner->continuation_destroys;
        delete continuation;
    }

    ScriptStepResult invokeAsync(
        void* context,
        lux_script_call_frame&,
        ScriptStepContext& step,
        ScriptBackendContinuation& output
    ) noexcept
    {
        auto& call = *static_cast<PreparedCall*>(context);
        ++call.instance->owner->tick_calls;
        auto awaiting = step.awaitables.create();
        if (!awaiting)
            return ScriptStepResult::failed(51);
        auto* continuation = new (std::nothrow) Continuation{call.instance->owner, call.instance};
        if (continuation == nullptr)
        {
            step.awaitables.discard(awaiting->id);
            return ScriptStepResult::failed(52);
        }
        call.instance->owner->completions.push_back(awaiting->completion);
        output = {continuation, &resumeAsync, &destroyContinuation};
        return ScriptStepResult::suspended(awaiting->id);
    }

    struct Harness final
    {
        Harness(
            std::size_t mount_count,
            bool entity_scope,
            bool begin,
            bool end,
            bool invalid_begin = false,
            bool invalid_end = false
        )
            : simulation(makeSimulation()), artifact(makeArtifact(begin, end, invalid_begin, invalid_end)),
              entity_scope(entity_scope)
        {
            std::vector<ScriptRuntimeMount> builder;
            for (std::size_t index{}; index < mount_count; ++index)
            {
                if (entity_scope)
                {
                    entities.push_back(registry.create());
                }
                builder.push_back({ScriptMountId{index + 1U}, assetId(),
                    entity_scope ? ScriptInstanceScope{EntityScriptScope{entities.back()}}
                                 : ScriptInstanceScope{SimulationScriptScope{}}, {{kTick, HookScriptTarget{kSystem,
                                     kHook}}}});
            }
            auto built = std::optional{std::move(builder)};
            assert(built);
            description = std::move(*built);
            for (std::size_t index{}; index < description.size(); ++index)
                description[index].configuration_index = static_cast<std::uint32_t>(index);
            assert(hook.prepare(1U) == EEndpointMutationError::NONE);
            bridge = std::make_unique<ScriptHookEndpoint<void()>>(kSystem, kHook, hook);
            endpoint = bridge->descriptor();
            backend_state.hook = std::addressof(hook);
            backend = {
                lux::rdesc::Script::Kind::CPP_STATIC,
                &backend_state,
                &createInstance,
                &prepareMethod,
                &releaseMethod,
                &destroyInstance
            };
        }

        [[nodiscard]] lux::cxx::expected<ScriptSystem,
            EScriptSystemError> create(std::size_t initial_count = 16U) noexcept
        {
            return ScriptSystem::create(
                simulation,
                *planScriptRuntimeCapacity(description),
                std::span{description}.first((std::min)(initial_count, description.size())),
                registry,
                clock,
                ScriptRuntimeLimits{32U, 16U, 16U, 8U, 16U, 16U, 64U, 16U, 16U, 16U, 16U, 16U},
                {this, &resolveArtifact},
                {},
                std::span{&backend, 1U},
                std::span{&endpoint, 1U},
                {}
            );
        }

        void submitEntity(ScriptSystem& system, std::size_t index)
        {
            std::array<ScriptMountStatus, 16U> changes;
            assert(system.collectMountStatusChanges(changes));
            description[index].scope = EntityScriptScope{entities[index]};
            assert(system.mountResolvedBatch(std::span{&description[index], 1U}));
        }

        static bool resolveArtifact(
            void* context,
            const lux::asset::AssetId& requested,
            ResolvedScriptArtifact& output
        ) noexcept
        {
            auto& self = *static_cast<Harness*>(context);
            if (requested != assetId())
                return false;
            output.artifact = self.invalid_artifact ? nullptr : &self.artifact;
            output.lease = &self;
            output.release = [](void* lease) noexcept { ++static_cast<Harness*>(lease)->leases_released; };
            ++self.leases_acquired;
            return true;
        }



        SimulationDescription simulation;
        lux::script::ScriptArtifact artifact;
        std::vector<ScriptRuntimeMount> description;
        SimulationClock clock;
        Registry registry;
        HookPoint<void()> hook;
        std::unique_ptr<ScriptHookEndpoint<void()>> bridge;
        ScriptHookEndpointDescriptor endpoint;
        BackendState backend_state;
        ScriptBackendDescriptor backend;
        std::vector<Entity> entities;
        bool entity_scope{};
        bool invalid_artifact{};
        std::size_t leases_acquired{};
        std::size_t leases_released{};
    };

    void testPendingDoesNotMaskFatal()
    {
        Harness harness{3U, true, true, true};
        auto created = harness.create();
        assert(created);
        auto system = std::move(*created);
        assert(system.prepare());
        harness.registry.destroy(harness.entities[0]);
        harness.entities[0] = NullEntity;
        for (unsigned step{}; step < 8U; ++step)
        {
            assert(system.processLifecycle());
            assert(system.executeStablePoint());
            assert(system.activeInstanceCount() == 2U);
            assert(harness.backend_state.begins == 3U);
        }
        harness.entities[0] = harness.registry.create();
        harness.submitEntity(system, 0U);
        assert(system.executeStablePoint());
        assert(harness.backend_state.begins == 4U);
        assert(system.activeInstanceCount() == 3U);

        // Queue a pending candidate before another candidate whose BeginPlay will fail.
        harness.registry.destroy(harness.entities[0]);
        harness.entities[0] = NullEntity;
        harness.registry.destroy(harness.entities[1]);
        harness.entities[1] = harness.registry.create();
        harness.submitEntity(system, 1U);
        harness.backend_state.fail_begin_serial = harness.backend_state.creates + 1U;
        const auto result = system.executeStablePoint();
        assert(!result && result.error() == EScriptSystemError::INVOCATION_FAILURE);
        assert(system.activeInstanceCount() == 1U);
        assert(system.shutdown());
        assert(harness.backend_state.destroys == harness.backend_state.creates);
    }

    void testOwnedRuntimeInput()
    {
        Harness harness{1U, false, true, true};
        auto created = harness.create();
        assert(created);
        harness.description.clear();
        harness.description.shrink_to_fit();
        assert(created->prepare());
        assert(dispatchHookForTest(harness.hook) == 1U);
        assert(harness.backend_state.begins == 1U && harness.backend_state.tick_calls == 1U);
        assert(created->shutdown());
        assert(harness.backend_state.ends == 1U && harness.backend_state.destroys == 1U);
    }

    void testResolvedBatchProtocol()
    {
        Harness harness{3U, true, true, true};
        auto created = harness.create(1U);
        assert(created && created->prepare());
        auto& system = *created;
        assert(harness.backend_state.begins == 1U);
        auto* first_host = harness.backend_state.hosts.front();
        const auto initial = system.queryMountStatus({1U});
        assert(initial && *initial && (**initial).state == EScriptMountState::ACTIVE);
        const auto repeated = system.queryMountStatus({1U});
        assert(repeated && (**repeated).revision == (**initial).revision);
        const auto untouched = system.collectMountStatusChanges({});
        assert(untouched && untouched->written == 0U && untouched->remaining == 1U);

        auto invalid_batch = harness.description;
        invalid_batch.erase(invalid_batch.begin());
        invalid_batch.back().bindings.front().symbol = lux::script::InvalidScriptSymbolId;
        const auto rejected = system.mountResolvedBatch(invalid_batch);
        assert(!rejected && rejected.error() == EScriptSystemError::INVALID_INPUT);
        const auto unknown = system.queryMountStatus({2U});
        assert(unknown && !*unknown);
        assert(harness.backend_state.creates == 1U);
        assert(system.collectMountStatusChanges({})->remaining == 1U);
        invalid_batch = harness.description;
        invalid_batch.erase(invalid_batch.begin());
        invalid_batch.back().bindings.push_back({kBegin, HookScriptTarget{kSystem, kHook}});
        const auto capacity_error = system.mountResolvedBatch(invalid_batch);
        assert(!capacity_error && capacity_error.error() == EScriptSystemError::CAPACITY_EXCEEDED);
        assert(!*system.queryMountStatus({2U}));
        assert(system.collectMountStatusChanges({})->remaining == 1U);


        assert(system.mountResolvedBatch(std::span{harness.description}.subspan(1U)));
        const auto accepted = system.queryMountStatus({2U});
        assert(accepted && *accepted && (**accepted).state == EScriptMountState::INACTIVE);
        assert((**accepted).submission_state == EScriptMountSubmissionState::ACCEPTED);
        assert(harness.backend_state.creates == 1U);
        std::array<ScriptMountStatus, 1U> one;
        auto first = system.collectMountStatusChanges(one);
        assert(first && first->written == 1U && first->remaining == 2U && one[0].id.value == 1U);
        assert(system.processLifecycle());
        assert(harness.backend_state.creates == 3U && harness.backend_state.begins == 3U);
        assert(first_host == harness.backend_state.hosts.front());
        assert(first_host->self() == harness.entities.front());
        assert(dispatchHookForTest(harness.hook) == 1U && harness.backend_state.tick_calls == 3U);

        harness.registry.destroy(harness.entities[1]);
        harness.entities[1] = harness.registry.create();
        harness.description[1].scope = EntityScriptScope{harness.entities[1]};
        const auto busy = system.mountResolvedBatch(std::span{&harness.description[1], 1U});
        assert(!busy && busy.error() == EScriptSystemError::ENDPOINT_BUSY);
        std::array<ScriptMountStatus, 3U> changes;
        assert(system.collectMountStatusChanges(changes));
        assert(system.mountResolvedBatch(std::span{&harness.description[1], 1U}));
        const auto replacing = system.queryMountStatus({2U});
        assert(replacing && *replacing && (**replacing).state == EScriptMountState::RETIRING);
        assert(!(**replacing).reclaimed);
        assert((**replacing).submission_state == EScriptMountSubmissionState::ACCEPTED);
        assert(std::get<EntityScriptScope>((**replacing).submitted_scope).self == harness.entities[1]);
        assert(harness.backend_state.destroys == 0U);
        assert(system.processLifecycle(EScriptLifecycleAdmission::RETIRE_ONLY));
        assert(harness.backend_state.destroys == 1U && harness.backend_state.creates == 3U);
        assert(system.processLifecycle());
        assert(harness.backend_state.creates == 4U && system.activeInstanceCount() == 3U);
        assert(system.shutdown());
        assert(harness.backend_state.destroys == harness.backend_state.creates);
        assert(system.collectMountStatusChanges(changes));
        assert(system.queryMountStatus({2U}));
        const auto closed = system.mountResolvedBatch({});
        assert(!closed && closed.error() == EScriptSystemError::SHUT_DOWN);
    }

    void testResolvedInputExpiryAndReuse()
    {
        Harness harness{2U, true, true, true};
        auto created = harness.create(0U);
        assert(created && created->prepare());
        auto& system = *created;
        assert(system.mountResolvedBatch(std::span{harness.description}.first(1U)));
        harness.registry.destroy(harness.entities[0]);
        assert(system.processLifecycle());
        assert(harness.backend_state.creates == 0U);
        auto expired = system.queryMountStatus({1U});
        assert(expired && *expired && (**expired).submission_state == EScriptMountSubmissionState::REJECTED);
        assert((**expired).submission_error == EScriptSystemError::INVALID_INPUT && (**expired).reclaimed);
        std::array<ScriptMountStatus, 2U> changes;
        assert(system.collectMountStatusChanges(changes));
        const auto backing = system.stats();
        for (std::size_t iteration{}; iteration < 64U; ++iteration)
        {
            harness.entities[0] = harness.registry.create();
            harness.description[0].scope = EntityScriptScope{harness.entities[0]};
            assert(system.mountResolvedBatch(std::span{harness.description}.first(1U)));
            assert(system.processLifecycle());
            assert(system.activeInstanceCount() == 1U);
            const auto current = system.stats();
            assert(current.configured_mounts == 1U && current.pending_mounts == 0U);
            assert(current.mount_backing_bytes == backing.mount_backing_bytes);
            assert(current.method_backing_bytes == backing.method_backing_bytes);
            assert(current.binding_backing_bytes == backing.binding_backing_bytes);
            assert(current.mount_feedback_backing_bytes == backing.mount_feedback_backing_bytes);
            assert(system.collectMountStatusChanges(changes));
            harness.registry.destroy(harness.entities[0]);
            assert(system.processLifecycle());
            assert(system.activeInstanceCount() == 0U);
            assert(harness.backend_state.creates == iteration + 1U);
            assert(harness.backend_state.destroys == iteration + 1U);
            assert(harness.backend_state.releases == harness.backend_state.prepares);
            assert(system.collectMountStatusChanges(changes));
        }
        harness.description[0].scope = EntityScriptScope{harness.registry.create()};
        auto changed = harness.description[0];
        changed.bindings.clear();
        const auto shape_error = system.mountResolvedBatch(std::span{&changed, 1U});
        assert(!shape_error && shape_error.error() == EScriptSystemError::INVALID_INPUT);
        assert(system.mountResolvedBatch(std::span{&harness.description[1], 1U}));
        assert(system.processLifecycle());
        assert(system.activeInstanceCount() == 1U);
        assert(system.shutdown());
        assert(harness.backend_state.creates == 65U && harness.backend_state.destroys == 65U);
    }

    void testResolvedAssetFailure(bool mixed_batch)
    {
        Harness harness{mixed_batch ? 2U : 1U, true, true, true};
        std::array<std::uint8_t, 16U> missing_bytes{};
        missing_bytes[0] = 0x76U;
        harness.description[0].asset = lux::asset::AssetId{missing_bytes};
        auto created = harness.create(0U);
        assert(created && created->prepare());
        auto& system = *created;
        assert(system.mountResolvedBatch(harness.description));
        const auto accepted = system.queryMountStatus({1U});
        assert(accepted && *accepted);
        assert((**accepted).submission_state == EScriptMountSubmissionState::ACCEPTED);
        assert(harness.backend_state.creates == 0U);

        const auto processed = system.processLifecycle();
        assert(!processed && processed.error() == EScriptSystemError::ASSET_NOT_RESIDENT);
        const auto failed = system.queryMountStatus({1U});
        assert(failed && *failed);
        assert((**failed).state == EScriptMountState::FAULTED);
        assert((**failed).submission_state == EScriptMountSubmissionState::REJECTED);
        assert((**failed).submission_error == EScriptSystemError::ASSET_NOT_RESIDENT);
        assert((**failed).reclaimed);
        assert(!(**failed).instance.valid());
        assert(system.stats().pending_mounts == 0U);
        const auto successes = mixed_batch ? 1U : 0U;
        assert(system.activeInstanceCount() == successes);
        assert(harness.backend_state.creates == successes && harness.backend_state.begins == successes);
        assert(harness.backend_state.ends == 0U && harness.backend_state.destroys == 0U);
        assert(harness.backend_state.releases == 0U && harness.backend_state.continuation_destroys == 0U);
        assert(harness.backend_state.prepares == 3U * successes);
        assert(harness.leases_acquired == successes && harness.leases_released == 0U);
        const auto repeated = system.queryMountStatus({1U});
        assert(repeated && *repeated && (**repeated).revision == (**failed).revision);
        const auto zero = system.collectMountStatusChanges({});
        assert(zero && zero->written == 0U && zero->remaining == harness.description.size());
        std::array<ScriptMountStatus, 1U> output;
        const auto partial = system.collectMountStatusChanges(output);
        assert(partial && partial->written == 1U && partial->remaining == successes);
        assert(output[0].id.value == 1U && output[0].reclaimed);
        assert(output[0].submission_state == EScriptMountSubmissionState::REJECTED);
        if (mixed_batch)
        {
            const auto normal = system.queryMountStatus({2U});
            assert(normal && *normal && (**normal).state == EScriptMountState::ACTIVE);
            const auto remaining = system.collectMountStatusChanges(output);
            assert(remaining && remaining->written == 1U && remaining->remaining == 0U);
            assert(output[0].id.value == 2U);
            assert(dispatchHookForTest(harness.hook) == 1U && harness.backend_state.tick_calls == 1U);
        }
        const auto empty = system.collectMountStatusChanges(output);
        assert(empty && empty->written == 0U && empty->remaining == 0U);
        assert(system.processLifecycle());
        assert(harness.backend_state.creates == successes);
        const auto still_failed = system.queryMountStatus({1U});
        assert(still_failed && *still_failed && (**still_failed).state == EScriptMountState::FAULTED);
        assert((**still_failed).reclaimed);
        assert(system.shutdown());
        assert(system.activeInstanceCount() == 0U);
        assert(harness.backend_state.ends == successes && harness.backend_state.destroys == successes);
        assert(harness.backend_state.releases == harness.backend_state.prepares);
        assert(harness.backend_state.continuation_destroys == 0U);
        assert(harness.leases_acquired == harness.leases_released);
    }

    void testResolvedPreparationFailures()
    {
        for (std::size_t failure{}; failure < 5U; ++failure)
        {
            Harness harness{1U, true, true, true};
            harness.invalid_artifact = failure == 0U;
            if (failure == 1U)
                harness.backend.kind = lux::rdesc::Script::Kind::LUA_SOURCE;
            harness.backend_state.fail_create = failure == 2U;
            harness.backend_state.fail_prepare_ordinal = failure == 3U ? 2U : 0U;
            harness.backend_state.fail_begin_serial = failure == 4U ? 1U : 0U;
            auto created = harness.create(0U);
            assert(created && created->prepare());
            auto& system = *created;
            assert(system.mountResolvedBatch(harness.description));
            const auto processed = system.processLifecycle();
            constexpr std::array expected_errors{
                EScriptSystemError::INVALID_ASSET, EScriptSystemError::BACKEND_NOT_AVAILABLE,
                EScriptSystemError::BACKEND_FAILURE, EScriptSystemError::BACKEND_FAILURE,
                EScriptSystemError::INVOCATION_FAILURE
            };
            assert(!processed && processed.error() == expected_errors[failure]);
            const auto status = system.queryMountStatus({1U});
            assert(status && *status && (**status).state == EScriptMountState::FAULTED);
            assert((**status).submission_state == EScriptMountSubmissionState::REJECTED);
            assert((**status).submission_error == expected_errors[failure] && (**status).reclaimed);
            assert(system.stats().pending_mounts == 0U && system.activeInstanceCount() == 0U);
            const auto expected_creates = failure >= 3U ? 1U : 0U;
            const auto expected_prepares = failure == 3U ? 1U : failure == 4U ? 3U : 0U;
            assert(harness.backend_state.creates == expected_creates);
            assert(harness.backend_state.destroys == expected_creates);
            assert(harness.backend_state.prepares == expected_prepares);
            assert(harness.backend_state.releases == expected_prepares);
            assert(harness.backend_state.begins == 0U && harness.backend_state.ends == 0U);
            assert(harness.leases_acquired == 1U && harness.leases_released == 1U);
            assert(system.processLifecycle() && system.shutdown());
            assert(harness.backend_state.destroys == expected_creates);
            assert(harness.backend_state.releases == expected_prepares);
            assert(harness.backend_state.ends == 0U && harness.leases_released == 1U);
        }
    }

    void testInitialLifecycle()
    {
        Harness harness{3U, false, true, true};
        auto created = harness.create();
        assert(created);
        auto system = std::move(*created);
        assert(system.prepare());
        assert(harness.backend_state.creates == 3U);
        assert(harness.backend_state.first_begin_create_count == 3U);
        assert(harness.backend_state.begins == 3U);
        assert(system.activeInstanceCount() == 3U);
        assert(dispatchHookForTest(harness.hook) == 1U);
        assert(system.shutdown());
        assert(harness.backend_state.ends == 3U);
        assert(harness.backend_state.destroys == 3U);
        assert(harness.backend_state.releases == harness.backend_state.prepares);
        assert(std::ranges::all_of(harness.backend_state.end_reasons, [](auto reason) {
            return reason == EScriptEndPlayReason::RUNTIME_STOPPED;
        }));
    }

    void testOptionalAndFailureLifecycle()
    {
        Harness none{1U, false, false, false};
        auto none_created = none.create();
        assert(none_created);
        auto none_system = std::move(*none_created);
        assert(none_system.prepare());
        assert(none_system.shutdown());
        assert(none.backend_state.begins == 0U && none.backend_state.ends == 0U);

        Harness begin_only{1U, false, true, false};
        auto begin_only_created = begin_only.create();
        assert(begin_only_created);
        auto begin_only_system = std::move(*begin_only_created);
        assert(begin_only_system.prepare());
        assert(begin_only_system.shutdown());
        assert(begin_only.backend_state.begins == 1U);
        assert(begin_only.backend_state.ends == 0U);
        assert(begin_only.backend_state.destroys == 1U);

        Harness end_only{1U, false, false, true};
        auto end_only_created = end_only.create();
        assert(end_only_created);
        auto end_only_system = std::move(*end_only_created);
        assert(end_only_system.prepare());
        assert(end_only_system.shutdown());
        assert(end_only.backend_state.begins == 0U);
        assert(end_only.backend_state.ends == 1U);
        assert(end_only.backend_state.destroys == 1U);

        Harness begin_failure{3U, false, true, true};
        begin_failure.backend_state.fail_begin_serial = 2U;
        auto begin_created = begin_failure.create();
        assert(begin_created);
        auto begin_system = std::move(*begin_created);
        const auto prepared = begin_system.prepare();
        assert(!prepared && prepared.error() == EScriptSystemError::INVOCATION_FAILURE);
        assert(begin_failure.backend_state.creates == 3U);
        assert(begin_failure.backend_state.begins == 1U);
        assert(begin_failure.backend_state.ends == 1U);
        assert(begin_failure.backend_state.destroys == 3U);
        assert(begin_system.shutdown());

        Harness end_failure{1U, false, true, true};
        end_failure.backend_state.fail_end = true;
        auto end_created = end_failure.create();
        assert(end_created);
        auto end_system = std::move(*end_created);
        assert(end_system.prepare());
        assert(end_system.shutdown());
        assert(end_failure.backend_state.ends == 1U);
        assert(end_failure.backend_state.destroys == 1U);
        assert(!end_failure.backend_state.end_reasons.empty());

        Harness normal_failure{1U, false, true, true};
        normal_failure.backend_state.fail_tick_serial = 1U;
        auto normal_created = normal_failure.create();
        assert(normal_created);
        auto normal_system = std::move(*normal_created);
        assert(normal_system.prepare());
        assert(dispatchHookForTest(normal_failure.hook) == 1U);
        assert(normal_system.activeInstanceCount() == 0U);
        assert(normal_system.executeStablePoint());
        assert(normal_failure.backend_state.ends == 1U);
        assert(normal_failure.backend_state.destroys == 1U);
        assert(normal_failure.backend_state.end_reasons.front() == EScriptEndPlayReason::FAULTED);
        assert(normal_system.shutdown());
        assert(normal_failure.backend_state.ends == 1U);
        assert(normal_failure.backend_state.destroys == 1U);
    }

    void testSignatureValidation()
    {
        Harness invalid_begin{1U, false, true, false, true, false};
        auto begin_created = invalid_begin.create();
        assert(begin_created);
        auto begin_system = std::move(*begin_created);
        const auto begin_prepared = begin_system.prepare();
        assert(!begin_prepared && begin_prepared.error() == EScriptSystemError::SIGNATURE_MISMATCH);
        assert(begin_system.shutdown());

        Harness invalid_end{1U, false, false, true, false, true};
        auto end_created = invalid_end.create();
        assert(end_created);
        auto end_system = std::move(*end_created);
        const auto end_prepared = end_system.prepare();
        assert(!end_prepared && end_prepared.error() == EScriptSystemError::SIGNATURE_MISMATCH);
        assert(end_system.shutdown());
    }

    void testIncarnationAndPendingContinuation()
    {
        Harness harness{2U, true, true, true};
        harness.backend_state.async_tick = true;
        auto created = harness.create();
        assert(created);
        auto system = std::move(*created);
        assert(system.prepare());
        assert(dispatchHookForTest(harness.hook) == 1U);
        assert(system.activeContinuationCount() == 2U);
        const auto late_completion = harness.backend_state.completions.front();

        for (const auto entity : harness.entities)
            harness.registry.destroy(entity);
        for (auto& entity : harness.entities)
            entity = harness.registry.create();
        for (std::size_t index{}; index < harness.entities.size(); ++index)
            harness.submitEntity(system, index);
        assert(system.executeStablePoint());
        assert(harness.backend_state.ends == 2U);
        assert(harness.backend_state.destroys == 2U);
        assert(harness.backend_state.continuation_destroys == 2U);
        assert(std::ranges::all_of(harness.backend_state.end_normal_dispatches, [](std::size_t count) {
            return count == 0U;
        }));
        assert(harness.backend_state.begins == 4U);
        assert(system.activeInstanceCount() == 2U);
        const auto late = late_completion.ready();
        assert(!late && late.error() == EScriptAwaitableCompletionError::INVALID_ID);
        assert(harness.backend_state.resumes == 0U);

        assert(system.shutdown());
        assert(harness.backend_state.ends == 4U);
        assert(harness.backend_state.destroys == 4U);
        assert(std::count(
            harness.backend_state.end_reasons.begin(),
            harness.backend_state.end_reasons.end(),
            EScriptEndPlayReason::ENTITY_DESTROYED
        ) == 2);
    }
} // namespace

int main()
{
    testResolvedAssetFailure(true);
    testResolvedAssetFailure(false);
    testResolvedPreparationFailures();
    testOwnedRuntimeInput();
    testResolvedBatchProtocol();
    testResolvedInputExpiryAndReuse();
    testInitialLifecycle();
    testOptionalAndFailureLifecycle();
    testSignatureValidation();
    testIncarnationAndPendingContinuation();
    testPendingDoesNotMaskFatal();
    return 0;
}

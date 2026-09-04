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

    [[nodiscard]] lux::world::WorldObjectId objectId(std::uint8_t value) noexcept
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[0] = 0x76U;
        bytes[15] = value;
        return lux::world::WorldObjectId{uuids::uuid{bytes}};
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
        bool fail_end{};
        bool async_tick{};
        HookPoint<void()>* hook{};
        std::vector<std::uint32_t> end_values;
        std::vector<EScriptEndPlayReason> end_reasons;
        std::vector<std::size_t> end_normal_dispatches;
        std::vector<ScriptAwaitableCompletion> completions;
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
                static_cast<void>(state.hook->dispatch());
            state.end_normal_dispatches.push_back(state.tick_calls - calls_before);
            return state.fail_end ? 41 : 0;
        }
        return 42;
    }

    EScriptBackendResult createInstance(
        void* context,
        const ScriptInstanceCreateContext&,
        const lux::script::ScriptArtifact&,
        ScriptBackendInstance& output
    ) noexcept
    {
        auto& state = *static_cast<BackendState*>(context);
        auto* instance = new (std::nothrow) Instance{&state, state.creates + 1U, 0U};
        if (instance == nullptr)
            return EScriptBackendResult::ALLOCATION_FAILURE;
        ++state.creates;
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
            ScriptSystemDescriptionBuilder builder;
            for (std::size_t index{}; index < mount_count; ++index)
            {
                if (entity_scope)
                {
                    objects.push_back(objectId(static_cast<std::uint8_t>(index + 1U)));
                    entities.push_back(registry.create());
                }
                assert(builder.addMount({
                    ScriptMountId{index + 1U},
                    assetId(),
                    entity_scope ? ScriptMountScope{EntityScriptMount{objects.back()}}
                                 : ScriptMountScope{SimulationScriptMount{}},
                    true,
                    {{kTick, HookScriptTarget{kSystem, kHook}}}
                }));
            }
            auto built = std::move(builder).build(simulation);
            assert(built);
            description = std::move(*built);
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

        [[nodiscard]] lux::cxx::expected<ScriptSystem, EScriptSystemError> create() noexcept
        {
            return ScriptSystem::create(
                simulation,
                description,
                registry,
                clock,
                ScriptRuntimeLimits{32U, 16U, 16U, 8U, 16U, 16U, 64U, 16U, 16U, 16U, 16U, 16U},
                {this, &resolveArtifact},
                entity_scope ? WorldObjectResolver{this, &resolveWorld} : WorldObjectResolver{},
                {},
                std::span{&backend, 1U},
                std::span{&endpoint, 1U},
                {}
            );
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
            output.artifact = &self.artifact;
            return true;
        }

        static bool resolveWorld(
            void* context,
            const lux::world::WorldObjectId& object,
            Entity& output
        ) noexcept
        {
            auto& self = *static_cast<Harness*>(context);
            const auto found = std::find(self.objects.begin(), self.objects.end(), object);
            if (found == self.objects.end())
                return false;
            const auto index = static_cast<std::size_t>(found - self.objects.begin());
            if (index >= self.entities.size() || !self.registry.valid(self.entities[index]))
                return false;
            output = self.entities[index];
            return true;
        }

        SimulationDescription simulation;
        lux::script::ScriptArtifact artifact;
        ScriptSystemDescription description;
        SimulationClock clock;
        Registry registry;
        HookPoint<void()> hook;
        std::unique_ptr<ScriptHookEndpoint<void()>> bridge;
        ScriptHookEndpointDescriptor endpoint;
        BackendState backend_state;
        ScriptBackendDescriptor backend;
        std::vector<lux::world::WorldObjectId> objects;
        std::vector<Entity> entities;
        bool entity_scope{};
    };

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
        assert(harness.hook.dispatch() == 1U);
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
        assert(normal_failure.hook.dispatch() == 1U);
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
        assert(harness.hook.dispatch() == 1U);
        assert(system.activeContinuationCount() == 2U);
        const auto late_completion = harness.backend_state.completions.front();

        for (const auto entity : harness.entities)
            harness.registry.destroy(entity);
        for (auto& entity : harness.entities)
            entity = harness.registry.create();
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
    testInitialLifecycle();
    testOptionalAndFailureLifecycle();
    testSignatureValidation();
    testIncarnationAndPendingContinuation();
    return 0;
}

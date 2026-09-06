#include <lux/engine/simulation/scripting/lua/LuaScriptAbilityProjection.hpp>
#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using namespace lux::simulation::script;
    lux::script::lua::ELuaExecutionPolicy policy{lux::script::lua::ELuaExecutionPolicy::DEFAULT};

    struct Dispatch final
    {
        void (*call)(void*) noexcept;
    };

    int call(lua_State* state) noexcept
    {
        return detail::invokeLuaAbility<void>(state, [](detail::LuaPreparedAbilityAccess& access) noexcept {
            static_cast<const Dispatch*>(access.dispatch)->call(access.context);
        });
    }

    lux::script::ScriptAbilityErasedCallResult erased(
        void*,
        const void*,
        std::span<const lux::script::ScriptAbilityInputSlot>,
        std::span<lux::script::ScriptAbilityOutputSlot>
    ) noexcept
    {
        return {};
    }

    struct Ability final
    {
        Ability(std::string_view contract, std::string_view name, std::string_view method)
            : methods{{{lux::script::ScriptApiMethodIdView{method}, "call", "Call",
                        lux::script::EScriptApiMethodKind::COMMAND, {}, {}}}},
              description{lux::script::ScriptApiContractIdView{contract}, name, name, 1U,
                  lux::script::scriptAbilitySchemaHash(lux::script::ScriptApiContractIdView{contract},
                      lux::script::EScriptAbilityReceiverKind::PROVIDER_INSTANCE, methods),
                  lux::script::EScriptAbilityReceiverKind::PROVIDER_INSTANCE, methods},
              projections{{{methods.front().id, &call}}},
              bindings{{{methods.front().id, methods.front().kind, {}, {}, &erased, nullptr}}}
        {
        }

        std::array<lux::script::ScriptAbilityMethodDescription, 1U> methods;
        lux::script::ScriptAbilityDescription description;
        std::array<lux::script::lua::ScriptAbilityLuaMethodProjection, 1U> projections;
        std::array<lux::script::ScriptAbilityErasedMethodBinding, 1U> bindings;
    };

    lux::script::ScriptArtifact makeArtifact(const Ability& ability, std::string_view source)
    {
        lux::rdesc::Script description;
        description.module_name = ability.description.name;
        description.body = lux::rdesc::LuaSourceScript{"Object"};
        description.api_requirements.push_back({
            lux::script::ScriptApiContractId{ability.description.id.name()}, ability.description.schema_hash
        });
        description.exports = {{"save", 1U, {}, {}}, {"own", 2U, {}, {}}, {"foreign", 3U, {}, {}}};
        const auto bytes = std::as_bytes(std::span{source.data(), source.size()});
        auto result = lux::script::ScriptArtifact::create(
            std::move(description), std::vector<std::byte>{bytes.begin(), bytes.end()}
        );
        assert(result);
        return std::move(*result);
    }

    lux::asset::AssetId assetId(std::uint8_t number)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes.front() = number;
        return lux::asset::AssetId{bytes};
    }
}

void testAbilityProvenance()
{
    Ability alpha{"lux.test.provenance.alpha", "Alpha", "lux.test.provenance.alpha.call"};
    Ability beta{"lux.test.provenance.beta", "Beta", "lux.test.provenance.beta.call"};
    const std::array contributions{
        lux::script::lua::ScriptAbilityLuaContribution{&alpha.description, alpha.projections},
        lux::script::lua::ScriptAbilityLuaContribution{&beta.description, beta.projections}
    };
    auto backend = LuaScriptBackend::create({
        .instance_capacity = 4U,
        .prepared_call_capacity = 8U,
        .continuation_capacity = 1U,
        .execution_depth_capacity = 4U,
        .ability_catalog_method_capacity = 2U,
        .prepared_ability_capacity = 4U,
        .abilities = contributions,
        .execution_policy = policy,
        .prepared_ability_blocks = std::array{
            lux::simulation::script::LuaPreparedBlockClass{
                1U,
                4U
            }
        },
        .prepared_ability_storage_bytes =
            64U * 1024U * 1024U
    });
    assert(backend);
    const auto runtime = backend->descriptor();
    const Dispatch dispatch{[](void* context) noexcept { ++*static_cast<int*>(context); }};
    int alpha_calls{};
    int beta_calls{};
    const auto capability = [&](const Ability& ability, int& calls) {
        return PreparedScriptApiCapability{
            lux::script::ScriptApiContractId{ability.description.id.name()}, ability.description.schema_hash,
            &calls, &dispatch, 1U, ability.bindings
        };
    };
    auto alpha_capability = capability(alpha, alpha_calls);
    auto beta_capability = capability(beta, beta_calls);
    auto alpha_artifact = makeArtifact(alpha,
        "return {save=function() table.lux_saved= lux.Alpha.call end,"
        "own=function() lux.Alpha.call() end, foreign=function() table.lux_saved() end}"
    );
    auto beta_artifact = makeArtifact(beta,
        "return {save=function() end, own=function() lux.Beta.call() end,"
        "foreign=function() table.lux_saved() end}"
    );
    const auto create = [&](std::uint8_t asset, const lux::script::ScriptArtifact& artifact,
                            const PreparedScriptApiCapability& prepared) {
        ScriptBackendInstance instance;
        assert(runtime.createInstance(runtime.context,
            {assetId(asset), SimulationScriptScope{}, nullptr, {asset, 1U}, {&prepared, 1U}, {}},
            artifact, instance) == EScriptBackendResult::SUCCESS);
        return instance;
    };
    const auto invoke = [&](ScriptBackendInstance instance, const lux::script::ScriptArtifact& artifact,
                            std::size_t method) {
        ScriptBackendPreparedMethod prepared;
        assert(runtime.prepareMethod(runtime.context, instance, artifact.description().exports[method], prepared) ==
            EScriptBackendResult::SUCCESS);
        lux_script_call_frame frame{nullptr, 0U, 0U, nullptr, 0U, 0U, nullptr, prepared.synchronous.context};
        const auto result = prepared.synchronous.invoke(&frame);
        runtime.releaseMethod(runtime.context, instance, prepared);
        return result;
    };
    auto first = create(1U, alpha_artifact, alpha_capability);
    auto second = create(2U, beta_artifact, beta_capability);
    assert(invoke(first, alpha_artifact, 0U) == 0);
    assert(invoke(second, beta_artifact, 2U) != 0);
    assert(alpha_calls == 0 && beta_calls == 0);
    assert(invoke(second, beta_artifact, 1U) == 0);
    assert(beta_calls == 1);
    const auto same_capability = capability(alpha, beta_calls);
    auto same_prototype = create(1U, alpha_artifact, same_capability);
    assert(invoke(same_prototype, alpha_artifact, 2U) == 0);
    assert(alpha_calls == 0 && beta_calls == 2);
    runtime.destroyInstance(runtime.context, first);
    runtime.destroyInstance(runtime.context, same_prototype);
    auto rebuilt = create(3U, alpha_artifact, alpha_capability);
    assert(invoke(rebuilt, alpha_artifact, 2U) != 0);
    assert(alpha_calls == 0 && beta_calls == 2);
    runtime.destroyInstance(runtime.context, rebuilt);
    runtime.destroyInstance(runtime.context, second);
    assert(backend->stats().prepared_ability_slots == 0U);
}

void testEventProvenance()
{
    const std::array sources{
        lux::script::ScriptEventSourceDescription{"Source", "alpha", 1U, 1U,
            lux::script::EScriptEventRoute::SIMULATION_BROADCAST,
            {"lux.i32", lux::semantic::typeId("lux.i32"), LUX_SCRIPT_VK_INT32, 4U, 4U}, 1U, 1U, 1U, 1U, 1U},
        lux::script::ScriptEventSourceDescription{"Source", "beta", 2U, 2U,
            lux::script::EScriptEventRoute::SIMULATION_BROADCAST,
            {"lux.i32", lux::semantic::typeId("lux.i32"), LUX_SCRIPT_VK_INT32, 4U, 4U}, 1U, 1U, 2U, 2U, 1U}
    };
    auto backend = LuaScriptBackend::create({
        .instance_capacity = 2U,
        .prepared_call_capacity = 4U,
        .continuation_capacity = 2U,
        .execution_depth_capacity = 4U,
        .ability_catalog_method_capacity = 1U,
        .execution_policy = policy,
        .event_catalog_capacity = 2U,
        .prepared_event_capacity = 2U,
        .events = sources,
        .prepared_event_blocks = std::array{
            lux::simulation::script::LuaPreparedBlockClass{
                1U,
                2U
            }
        },
        .prepared_event_storage_bytes =
            64U * 1024U * 1024U
    });
    assert(backend);
    const auto make = [&](std::size_t index, std::string_view source) {
        lux::rdesc::Script description;
        description.module_name = sources[index].event_name;
        description.body = lux::rdesc::LuaSourceScript{"Object", {2U}};
        description.exports = {{"save", 1U, {}, {}}, {"wait", 2U, {}, {}}};
        description.event_requirements = {sources[index]};
        const auto bytes = std::as_bytes(std::span{source.data(), source.size()});
        auto created = lux::script::ScriptArtifact::create(
            std::move(description), std::vector<std::byte>{bytes.begin(), bytes.end()}
        );
        assert(created);
        return std::move(*created);
    };
    auto first_artifact = make(0U,
        "return {save=function() table.lux_event= lux.Event.Source.alpha end,"
        "wait=function() table.lux_event() end}"
    );
    auto second_artifact = make(1U,
        "return {save=function() end, wait=function() table.lux_event() end}"
    );
    const auto runtime = backend->descriptor();
    std::array<ScriptBackendInstance, 2U> instances;
    const std::array artifacts{&first_artifact, &second_artifact};
    for (std::size_t index{}; index < instances.size(); ++index)
    {
        const PreparedScriptEventAdmission event{&sources[index], {}, {}, {}};
        assert(runtime.createInstance(runtime.context,
            {assetId(static_cast<std::uint8_t>(index + 1U)), SimulationScriptScope{}, nullptr,
                {static_cast<std::uint32_t>(index + 1U), 1U}, {}, {&event, 1U}},
            *artifacts[index], instances[index]) == EScriptBackendResult::SUCCESS);
    }
    ScriptBackendPreparedMethod save;
    assert(runtime.prepareMethod(runtime.context, instances[0], first_artifact.description().exports[0], save) ==
        EScriptBackendResult::SUCCESS);
    lux_script_call_frame frame{nullptr, 0U, 0U, nullptr, 0U, 0U, nullptr, save.synchronous.context};
    assert(save.synchronous.invoke(&frame) == 0);
    runtime.releaseMethod(runtime.context, instances[0], save);
    ScriptBackendPreparedMethod wait;
    assert(runtime.prepareMethod(runtime.context, instances[1], second_artifact.description().exports[1], wait) ==
        EScriptBackendResult::SUCCESS);
    std::size_t registrations{};
    ScriptStepContext step{{2U, 1U}, &registrations, nullptr, nullptr,
        [](void* context, ScriptInstanceId, ScriptEventAdmissionHandle) noexcept
            -> lux::cxx::expected<ScriptAwaitableId, EScriptEventWaitError> {
            ++*static_cast<std::size_t*>(context);
            return ScriptAwaitableId{1U, 1U};
        }};
    ScriptBackendContinuation continuation;
    assert(wait.resumable.invoke(wait.resumable.context, frame, step, continuation).state == EScriptStepState::FAILED);
    assert(registrations == 0U);
    if (continuation)
        continuation.destroy(continuation.state);
    runtime.releaseMethod(runtime.context, instances[1], wait);
    for (auto instance : instances)
        runtime.destroyInstance(runtime.context, instance);
    assert(backend->stats().prepared_event_slots == 0U);
}

void testNestedScopes()
{
    Ability alpha{"lux.test.nested.alpha", "Alpha", "lux.test.nested.alpha.call"};
    Ability beta{"lux.test.nested.beta", "Beta", "lux.test.nested.beta.call"};
    const std::array contributions{
        lux::script::lua::ScriptAbilityLuaContribution{&alpha.description, alpha.projections},
        lux::script::lua::ScriptAbilityLuaContribution{&beta.description, beta.projections}
    };
    const lux::script::ScriptEventSourceDescription event{"Source", "ready", 1U, 1U,
        lux::script::EScriptEventRoute::SIMULATION_BROADCAST,
        {"lux.i32", lux::semantic::typeId("lux.i32"), LUX_SCRIPT_VK_INT32, 4U, 4U}, 1U, 1U, 1U, 1U, 1U};
    auto backend = LuaScriptBackend::create({
        .instance_capacity = 2U, .prepared_call_capacity = 4U, .continuation_capacity = 1U,
        .execution_depth_capacity = 4U, .ability_catalog_method_capacity = 2U, .prepared_ability_capacity = 2U,
        .abilities = contributions, .execution_policy = policy,
        .event_catalog_capacity = 1U, .prepared_event_capacity = 1U, .events = {&event, 1U},
        .prepared_ability_blocks = std::array{
            lux::simulation::script::LuaPreparedBlockClass{
                1U,
                2U
            }
        },
        .prepared_ability_storage_bytes =
            64U * 1024U * 1024U,
        .prepared_event_blocks = std::array{
            lux::simulation::script::LuaPreparedBlockClass{
                1U,
                1U
            }
        },
        .prepared_event_storage_bytes =
            64U * 1024U * 1024U
    });
    assert(backend);
    auto outer = makeArtifact(alpha,
        "return {save=function() end, foreign=function() end, own=function() lux.Alpha.call(); lux.Alpha.call() end}");
    auto inner_base = makeArtifact(beta,
        "return {own=function() lux.Beta.call() end, foreign=function() error('expected nested failure') end,"
        "save=function() lux.Beta.call(); lux.Event.Source.ready(); lux.Beta.call() end}");
    auto description = inner_base.description();
    std::get<lux::rdesc::LuaSourceScript>(description.body).suspension_capable_exports = {1U};
    description.event_requirements = {event};
    auto inner = lux::script::ScriptArtifact::create(std::move(description),
        std::vector<std::byte>{inner_base.payload().begin(), inner_base.payload().end()});
    assert(inner);
    struct Nested final
    {
        ScriptBackendPreparedMethod* method{};
        int calls{};
        int status{};
        std::size_t waits{};
        bool coroutine{};
        ScriptStepResult result;
        ScriptBackendContinuation continuation;
        static ScriptStepContext context(Nested& self) noexcept
        {
            return {{2U, 1U}, &self, nullptr, nullptr,
                [](void* opaque, ScriptInstanceId, ScriptEventAdmissionHandle) noexcept
                    -> lux::cxx::expected<ScriptAwaitableId, EScriptEventWaitError> {
                    ++static_cast<Nested*>(opaque)->waits;
                    return ScriptAwaitableId{1U, 1U};
                }};
        }
    } nested;
    int inner_calls{};
    const Dispatch outer_dispatch{[](void* opaque) noexcept {
        auto& self = *static_cast<Nested*>(opaque);
        if (++self.calls != 1)
            return;
        lux_script_call_frame frame{};
        if (self.coroutine)
        {
            auto step = Nested::context(self);
            self.result = self.method->resumable.invoke(self.method->resumable.context, frame, step, self.continuation);
        }
        else
        {
            frame.user_context = self.method->synchronous.context;
            self.status = self.method->synchronous.invoke(&frame);
        }
    }};
    const Dispatch inner_dispatch{[](void* opaque) noexcept { ++*static_cast<int*>(opaque); }};
    const PreparedScriptApiCapability outer_capability{
        lux::script::ScriptApiContractId{alpha.description.id.name()}, alpha.description.schema_hash,
        &nested, &outer_dispatch, 1U, alpha.bindings};
    const PreparedScriptApiCapability inner_capability{
        lux::script::ScriptApiContractId{beta.description.id.name()}, beta.description.schema_hash,
        &inner_calls, &inner_dispatch, 1U, beta.bindings};
    const auto runtime = backend->descriptor();
    ScriptBackendInstance outer_instance, inner_instance;
    const PreparedScriptEventAdmission prepared_event{&event, {}, {}, {}};
    assert(runtime.createInstance(runtime.context,
        {assetId(11U), SimulationScriptScope{}, nullptr, {1U, 1U}, {&outer_capability, 1U}, {}},
        outer, outer_instance) == EScriptBackendResult::SUCCESS);
    assert(runtime.createInstance(runtime.context,
        {assetId(12U), SimulationScriptScope{}, nullptr, {2U, 1U}, {&inner_capability, 1U}, {&prepared_event, 1U}},
        *inner, inner_instance) == EScriptBackendResult::SUCCESS);
    ScriptBackendPreparedMethod outer_method;
    assert(runtime.prepareMethod(runtime.context, outer_instance, outer.description().exports[1], outer_method) ==
        EScriptBackendResult::SUCCESS);
    for (std::size_t mode{}; mode < 3U; ++mode)
    {
        ScriptBackendPreparedMethod inner_method;
        const auto index = mode == 0U ? 1U : mode == 1U ? 2U : 0U;
        assert(runtime.prepareMethod(runtime.context, inner_instance,
            inner->description().exports[index], inner_method) ==
            EScriptBackendResult::SUCCESS);
        nested.method = &inner_method;
        nested.calls = 0;
        nested.coroutine = mode == 2U;
        lux_script_call_frame frame{};
        frame.user_context = outer_method.synchronous.context;
        assert(outer_method.synchronous.invoke(&frame) == 0 && nested.calls == 2);
        if (mode == 0U)
            assert(nested.status == 0 && inner_calls == 1);
        else if (mode == 1U)
            assert(nested.status != 0 && inner_calls == 1);
        else
        {
            assert(nested.result.state == EScriptStepState::SUSPENDED && nested.waits == 1U && inner_calls == 2);
            ScriptOwnedResumeValue value;
            value.type = makePreparedResumeType<std::int32_t>();
            assert(value.bytes.resize(4U));
            const std::int32_t payload{7};
            std::memcpy(value.bytes.data(), &payload, sizeof(payload));
            auto step = Nested::context(nested);
            const auto result = nested.continuation.resume(nested.continuation.state, step,
                {{1U, 1U}, EScriptAwaitableState::READY, &value, {}});
            assert(result.state == EScriptStepState::COMPLETED && inner_calls == 3);
            nested.continuation.destroy(nested.continuation.state);
        }
        runtime.releaseMethod(runtime.context, inner_instance, inner_method);
    }
    assert(backend->stats().execution_depth_high_water == 2U);
    runtime.releaseMethod(runtime.context, outer_instance, outer_method);
    runtime.destroyInstance(runtime.context, inner_instance);
    runtime.destroyInstance(runtime.context, outer_instance);
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view{argv[1]} == "--interpreter-only")
        policy = lux::script::lua::ELuaExecutionPolicy::INTERPRETER_ONLY;
    testAbilityProvenance();
    testEventProvenance();
    testNestedScopes();
}

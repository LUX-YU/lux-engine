#include "CppStaticScriptBridgeFixture.hpp"
#include "CppCoroutineAbility.hpp"
#include "CppCoroutineAbility.ability.generated.hpp"

#include <lux/engine/simulation/scripting/cpp_static/CppStaticScriptBridge.hpp>
#include <lux/engine/simulation/scripting/cpp_static/ScriptDelayCoroutine.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <optional>

namespace cpp_static_test_detail
{
    struct NonTrivialSemantic final
    {
        NonTrivialSemantic() = default;
        NonTrivialSemantic(const NonTrivialSemantic&) noexcept {}
        ~NonTrivialSemantic() noexcept {}
    };
}

namespace lux::semantic
{
    template <>
    struct TypeTraits<cpp_static_test_detail::NonTrivialSemantic> final
    {
        inline static constexpr std::string_view CanonicalName{"lux.test.NonTrivialSemantic"};
        inline static constexpr std::uint8_t AbiKind{LUX_SCRIPT_VK_STRUCT_REF};
        inline static constexpr std::uint32_t Size{sizeof(cpp_static_test_detail::NonTrivialSemantic)};
        inline static constexpr std::uint32_t Alignment{alignof(cpp_static_test_detail::NonTrivialSemantic)};
    };
}

namespace
{
    using namespace lux::simulation::script;
    using NonTrivialSemantic = cpp_static_test_detail::NonTrivialSemantic;

    bool resolveRecord(
        void*,
        const lux::meta::RefType& type,
        lux::semantic::Layout& result
    ) noexcept
    {
        const auto base = static_cast<lux::meta::EBaseType>(type.qtype.base);
        const auto* reflected = base == lux::meta::EBaseType::Record
            ? static_cast<const lux::meta::RefClass*>(type.ptr)
            : nullptr;
        if (reflected && reflected->full_name == "lux::simulation::test::BridgeRecord")
        {
            constexpr std::string_view name{"lux.test.BridgeRecord"};
            result = {
                lux::semantic::typeId(name),
                name,
                LUX_SCRIPT_VK_STRUCT_REF,
                sizeof(lux::simulation::test::BridgeRecord),
                alignof(lux::simulation::test::BridgeRecord)};
            return true;
        }
        if (type.hash == lux::cxx::type_hash<lux::simulation::script::EScriptEndPlayReason>())
        {
            using Reason = lux::simulation::script::EScriptEndPlayReason;
            using Traits = lux::semantic::TypeTraits<Reason>;
            result = {
                lux::semantic::typeId(Traits::CanonicalName),
                Traits::CanonicalName,
                Traits::AbiKind,
                Traits::Size,
                Traits::Alignment};
            return true;
        }
        return false;
    }

    [[nodiscard]] lux::asset::AssetId assetId()
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[0] = 0xA5U;
        return lux::asset::AssetId{bytes};
    }

    lux::cxx::expected<ScriptAwaitableRegistration, EScriptAwaitableCreateError> createAwaitable(
        void*,
        ScriptInstanceId,
        std::optional<lux::simulation::script::PreparedResumeType>
    ) noexcept
    {
        return ScriptAwaitableRegistration{{1U, 1U}, {}};
    }

    void discardAwaitable(void*, ScriptInstanceId, ScriptAwaitableId) noexcept
    {
    }

    lux::cxx::expected<ScriptAwaitableId, EScriptEventWaitError> waitEvent(
        void*,
        ScriptInstanceId,
        ScriptEventWaitRequest
    ) noexcept
    {
        return ScriptAwaitableId{2U, 1U};
    }

    struct DelayProvider final
    {
        lux::script::ScriptAbilityStartResult nextStep(
            lux::script::ScriptAbilityCompletion<void>
        ) noexcept
        {
            return {};
        }

        lux::script::ScriptAbilityStartResult seconds(
            double,
            lux::script::ScriptAbilityCompletion<void>
        ) noexcept
        {
            return {};
        }

        lux::script::ScriptAbilityStartResult simulationSeconds(
            double,
            lux::script::ScriptAbilityCompletion<void>
        ) noexcept
        {
            return {};
        }

        lux::script::ScriptAbilityStartResult realSeconds(
            double,
            lux::script::ScriptAbilityCompletion<void>
        ) noexcept
        {
            return {};
        }
    };

    inline std::int32_t generated_ability_value{};

    struct CoroutineAbilityProvider final
    {
        std::int32_t value{10};
        bool reject{};

        std::int32_t read(std::int32_t input) noexcept
        {
            return value + input;
        }

        void write(std::int32_t next) noexcept
        {
            value = next;
        }

        const std::int32_t& borrowed() noexcept
        {
            return value;
        }

        lux::script::ScriptAbilityStartResult run(
            std::int32_t,
            lux::script::ScriptAbilityCompletion<std::int32_t>
        ) noexcept
        {
            return reject
                ? lux::script::ScriptAbilityStartResult{
                    lux::cxx::unexpected<lux::script::ScriptAbilityOperationError>(
                        lux::script::ScriptAbilityOperationError{81}
                    )
                }
                : lux::script::ScriptAbilityStartResult{};
        }
    };

    struct CoroutineAbilityObject final
    {
        ScriptCoroutine execute(ScriptCoroutineContext& context) noexcept
        {
            using Ability = lux::simulation::test::CppCoroutineAbility;
            auto ability = context.ability<Ability>();
            if (!ability)
                co_return;
            const auto queried = ability->read(2);
            const auto borrowed = ability->borrowed();
            const auto written = ability->write(20);
            if (!queried || !borrowed || !written)
                co_return;
            generated_ability_value = *queried + *borrowed;
            generated_ability_value += co_await ability->run(4);
        }
    };
}
int main()
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;
    namespace test = lux::simulation::test;

    static_assert(script::detail::kCppCoroutineArgumentSupported<std::int32_t>);
    static_assert(script::detail::kCppCoroutineArgumentSupported<const std::int32_t&>);
    static_assert(!script::detail::kCppCoroutineArgumentSupported<std::int32_t&&>);
    static_assert(!script::detail::kCppCoroutineArgumentSupported<std::int32_t&>);
    static_assert(!script::detail::kCppCoroutineArgumentSupported<std::int32_t*>);
    static_assert(!script::detail::kCppCoroutineArgumentSupported<const NonTrivialSemantic&>);

    lux::meta::ReflectionRegistry::initRegistry();
    auto& registry = lux::meta::ReflectionRegistry::instance();
    const auto* reflected = registry.findClass(
        "lux::simulation::test::BridgeBehavior");
    assert(reflected && reflected->methods.size() == 9U);

    const lux::meta::RefMethod* value_method{};
    const lux::meta::RefMethod* record_method{};
    const lux::meta::RefMethod* throwing_method{};
    const lux::meta::RefMethod* begin_method{};
    const lux::meta::RefMethod* end_method{};
    const lux::meta::RefMethod* task_method{};
    const lux::meta::RefMethod* event_method{};
    const lux::meta::RefMethod* next_step_method{};
    const lux::meta::RefMethod* twice_method{};
    for (const auto& method : reflected->methods)
    {
        if (method.invokable.name == "onValue")
            value_method = &method;
        else if (method.invokable.name == "onRecord")
            record_method = &method;
        else if (method.invokable.name == "throwing")
            throwing_method = &method;
        else if (method.invokable.name == "admitToGameplay")
            begin_method = &method;
        else if (method.invokable.name == "leaveGameplay")
            end_method = &method;
        else if (method.invokable.name == "task")
            task_method = &method;
        else if (method.invokable.name == "waitForEvent")
            event_method = &method;
        else if (method.invokable.name == "waitForNextStep")
            next_step_method = &method;
        else if (method.invokable.name == "waitTwice")
            twice_method = &method;
        assert(method.invokable.name != "unmarkedHelper");
    }
    assert(value_method && record_method && throwing_method && begin_method && end_method && task_method &&
        event_method && next_step_method && twice_method);
    lux::semantic::Layout lifecycle_layout;
    assert(resolveRecord(nullptr, end_method->invokable.parameters.front().type, lifecycle_layout));
    assert(lifecycle_layout.type_id == lux::semantic::typeId(lifecycle_layout.canonical_name));
    assert(lifecycle_layout.size == end_method->invokable.parameters.front().type.size);
    assert(lifecycle_layout.alignment == end_method->invokable.parameters.front().type.alignment);

    const std::array selected{value_method, record_method, begin_method, end_method};
    const std::array symbols{
        lux::script::ScriptSymbolId{101U},
        lux::script::ScriptSymbolId{102U},
        lux::script::ScriptSymbolId{104U},
        lux::script::ScriptSymbolId{105U}};
    const auto coroutine = makeCppStaticCoroutineExport<
        &test::BridgeBehavior::task
    >(*task_method, lux::script::ScriptSymbolId{106U});
    const auto event_coroutine = makeCppStaticCoroutineExport<
        &test::BridgeBehavior::waitForEvent
    >(*event_method, lux::script::ScriptSymbolId{107U});
    const auto next_step_coroutine = makeCppStaticCoroutineExport<
        &test::BridgeBehavior::waitForNextStep
    >(*next_step_method, lux::script::ScriptSymbolId{108U});
    const auto twice_coroutine = makeCppStaticCoroutineExport<
        &test::BridgeBehavior::waitTwice
    >(*twice_method, lux::script::ScriptSymbolId{109U});
    const std::array coroutines{coroutine, event_coroutine, next_step_coroutine, twice_coroutine};
    const lux::script::ScriptEventSourceDescription event_source{
        "Gameplay",
        "damage",
        0x601U,
        0x602U,
        lux::script::EScriptEventRoute::SIMULATION_BROADCAST,
        {
            "lux.i32",
            lux::semantic::typeId("lux.i32"),
            LUX_SCRIPT_VK_INT32,
            sizeof(std::int32_t),
            alignof(std::int32_t)
        },
        0x603U,
        1U, 0x604U, 0x605U, 1U
    };
    auto typed_event = CppScriptEventSource<std::int32_t>::create(event_source);
    assert(typed_event);
    test::coroutine_event_source = std::move(*typed_event);
    using DelayTraits = lux::script::ScriptAbilityTraits<DelayAbility>;
    const std::array ability_requirements{
        lux::rdesc::ScriptApiRequirement{
            lux::script::ScriptApiContractId{DelayTraits::Description.id.name()},
            DelayTraits::Description.schema_hash
        }
    };
    const std::array event_requirements{event_source};
    auto projected = projectCppStaticEntityScript(
        "lux.test.bridge-behavior",
        "bridge-behavior-v1",
        *reflected,
        selected,
        symbols,
        CppStaticRecordSemanticResolver{nullptr, &resolveRecord},
        nullptr,
        {symbols[2], symbols[3]},
        coroutines,
        ability_requirements,
        event_requirements
    );
    assert(projected);
    assert(projected->description().schema_version == lux::rdesc::Script::kSchemaVersion);
    assert(projected->description().exports.size() == 8U);
    assert(projected->description().lifecycle.begin_play == symbols[2]);
    assert(projected->description().lifecycle.end_play == symbols[3]);
    assert(projected->description().exports[0].args[0].canonical_name ==
        "lux.f32");
    assert(projected->description().exports[1].args[0].canonical_name ==
        "lux.test.BridgeRecord");
    assert(projected->description().exports[1].args[0].pass ==
        lux::semantic::EValuePass::CONST_REF);
    assert(projected->description().exports[4].args[0].pass ==
        lux::semantic::EValuePass::CONST_REF);

    const auto invalid_lifecycle = projectCppStaticEntityScript(
        "lux.test.invalid-coroutine-lifecycle",
        "invalid-coroutine-lifecycle-v1",
        *reflected,
        selected,
        symbols,
        CppStaticRecordSemanticResolver{nullptr, &resolveRecord},
        nullptr,
        {coroutine.symbol, symbols[3]},
        coroutines,
        ability_requirements,
        event_requirements
    );
    assert(!invalid_lifecycle);

    const std::array throwing{throwing_method};
    const std::array throwing_symbols{lux::script::ScriptSymbolId{103U}};
    const auto rejected = projectCppStaticEntityScript(
        "lux.test.throwing",
        "throwing-v1",
        *reflected,
        throwing,
        throwing_symbols,
        {}
    );
    assert(!rejected);
    assert(rejected.error() ==
        ECppStaticScriptBridgeError::METHOD_NOT_NOEXCEPT);

    const std::array parameter_ids{lux::cxx::type_hash<std::int32_t>()};
    const auto* reflected_function = registry.findFunction(
        "lux::simulation::test::bridgeFreeFunction",
        parameter_ids);
    assert(reflected_function && reflected_function->is_noexcept);
    const std::array functions{reflected_function};
    const std::array function_symbols{lux::script::ScriptSymbolId{201U}};
    auto global = projectCppStaticGlobalScript(
        "lux.test.bridge-free",
        "bridge-free-v1",
        functions,
        function_symbols);
    assert(global);

    auto entity_asset_result = lux::script::ScriptArtifact::create(projected->description(), {});
    assert(entity_asset_result);
    auto entity_asset = std::move(*entity_asset_result);
    const std::array pools{CppStaticScriptPoolDescription{
        std::addressof(*projected),
        1U,
        2U,
        4096U,
        alignof(std::max_align_t),
        8U
    }};
    const std::array duplicate_pools{
        CppStaticScriptPoolDescription{std::addressof(*projected), 1U, 0U, 0U, alignof(std::max_align_t), 1U},
        CppStaticScriptPoolDescription{std::addressof(*projected), 1U, 0U, 0U, alignof(std::max_align_t), 1U}
    };
    auto duplicate_backend = CppStaticScriptBackend::create(duplicate_pools);
    assert(!duplicate_backend);
    auto backend_result = CppStaticScriptBackend::create(pools);
    assert(backend_result);
    auto backend = std::move(*backend_result);
    const auto descriptor = backend.descriptor();
    DelayProvider delay_provider;
    const auto delay_binding = lux::script::bindScriptAbility<DelayAbility>(delay_provider);
    assert(delay_binding.valid());
    const std::array capabilities{
        PreparedScriptApiCapability{
            lux::script::ScriptApiContractId{DelayTraits::Description.id.name()},
            DelayTraits::Description.schema_hash,
            delay_binding.context,
            delay_binding.dispatch,
            DelayTraits::Description.schema_version,
            delay_binding.erased_methods
        }
    };
    ScriptBehavior behavior;
    ScriptBackendInstance instance;
    assert(descriptor.createInstance(
        descriptor.context,
        ScriptInstanceCreateContext{
            assetId(),
            EntityScriptScope{ecs::Entity{1U}},
            &behavior,
            {1U, 1U},
            capabilities,
            event_requirements
        },
        entity_asset,
        instance
    ) == EScriptBackendResult::SUCCESS);

    ScriptBackendPreparedMethod call_method;
    assert(descriptor.prepareMethod(
        descriptor.context,
        instance,
        entity_asset.description().exports[0],
        call_method
    ) == EScriptBackendResult::SUCCESS);
    const auto call = call_method.synchronous;
    float value{3.5F};
    lux_script_value_slot argument{
        LUX_SCRIPT_VK_FLOAT,
        {},
        sizeof(value),
        lux::semantic::typeId("lux.f32"),
        &value};
    lux_script_call_frame frame{
        &argument, 1U, 0U, nullptr, 0U, 0U, nullptr, call.context};
    assert(call.invoke(&frame) == 0);
    assert(test::observed_value == value);

    ScriptBackendPreparedMethod begin_method_prepared;
    ScriptBackendPreparedMethod end_method_prepared;
    assert(descriptor.prepareMethod(
        descriptor.context,
        instance,
        entity_asset.description().exports[2],
        begin_method_prepared
    ) == EScriptBackendResult::SUCCESS);
    assert(descriptor.prepareMethod(
        descriptor.context,
        instance,
        entity_asset.description().exports[3],
        end_method_prepared
    ) == EScriptBackendResult::SUCCESS);
    const auto begin_call = begin_method_prepared.synchronous;
    const auto end_call = end_method_prepared.synchronous;
    lux_script_call_frame begin_frame{
        nullptr, 0U, 0U, nullptr, 0U, 0U, nullptr, begin_call.context};
    assert(begin_call.invoke(&begin_frame) == 0);
    frame.user_context = call.context;
    assert(call.invoke(&frame) == 0);
    const EScriptEndPlayReason end_reason{EScriptEndPlayReason::RUNTIME_STOPPED};
    lux_script_value_slot end_argument{
        LUX_SCRIPT_VK_UINT32,
        {},
        sizeof(end_reason),
        lux::semantic::typeId("lux.simulation.ScriptEndPlayReason"),
        const_cast<EScriptEndPlayReason*>(std::addressof(end_reason))};
    lux_script_call_frame end_frame{
        &end_argument, 1U, 0U, nullptr, 0U, 0U, nullptr, end_call.context};
    assert(end_call.invoke(&end_frame) == 0);
    assert(test::observed_lifecycle_value == 11);
    assert(test::observed_end_reason == end_reason);

    ScriptBackendPreparedMethod step_method;
    assert(descriptor.prepareMethod(
        descriptor.context,
        instance,
        entity_asset.description().exports[4],
        step_method
    ) == EScriptBackendResult::SUCCESS);
    const auto step_call = step_method.resumable;
    std::int32_t coroutine_input{5};
    lux_script_value_slot coroutine_argument{
        LUX_SCRIPT_VK_INT32,
        {},
        sizeof(coroutine_input),
        lux::semantic::typeId("lux.i32"),
        std::addressof(coroutine_input)
    };
    lux_script_call_frame coroutine_frame{
        std::addressof(coroutine_argument),
        1U,
        0U,
        nullptr,
        0U,
        0U,
        nullptr,
        nullptr
    };
    ScriptStepContext step_context{
        {1U, 1U},
        nullptr,
        &createAwaitable,
        &discardAwaitable
    };
    ScriptBackendContinuation continuation;
    const auto suspended = step_call.invoke(
        step_call.context,
        coroutine_frame,
        step_context,
        continuation
    );
    assert(suspended.state == EScriptStepState::SUSPENDED);
    assert(test::coroutine_value == 5);
    assert(continuation);
    coroutine_input = 999;
    const ScriptResumePacket packet{
        suspended.waiting_on,
        EScriptAwaitableState::READY,
        nullptr,
        {}
    };
    const auto completed = continuation.resume(continuation.state, step_context, packet);
    assert(completed.state == EScriptStepState::COMPLETED);
    assert(test::coroutine_value == 15);
    continuation.destroy(continuation.state);
    descriptor.releaseMethod(descriptor.context, instance, step_method);

    ScriptBackendPreparedMethod event_method_prepared;
    assert(descriptor.prepareMethod(
        descriptor.context,
        instance,
        entity_asset.description().exports[5],
        event_method_prepared
    ) == EScriptBackendResult::SUCCESS);
    const auto event_step_call = event_method_prepared.resumable;
    lux_script_call_frame empty_frame{};
    ScriptStepContext event_context{
        {1U, 1U},
        nullptr,
        &createAwaitable,
        &discardAwaitable,
        &waitEvent
    };
    ScriptBackendContinuation event_continuation;
    const auto event_suspended = event_step_call.invoke(
        event_step_call.context,
        empty_frame,
        event_context,
        event_continuation
    );
    assert(event_suspended.state == EScriptStepState::SUSPENDED);
    std::int32_t event_value{73};
    ScriptOwnedResumeValue owned_event;
    owned_event.type = lux::rdesc::ScriptValueType{
        "lux.i32",
        lux::semantic::typeId("lux.i32"),
        lux::semantic::EValuePass::VALUE,
        LUX_SCRIPT_VK_INT32,
        sizeof(event_value),
        alignof(std::int32_t)
    };
    assert(owned_event.bytes.resize(sizeof(event_value), alignof(std::int32_t)));
    std::memcpy(owned_event.bytes.data(), std::addressof(event_value), sizeof(event_value));
    const ScriptResumePacket event_packet{
        event_suspended.waiting_on,
        EScriptAwaitableState::READY,
        std::addressof(owned_event),
        {}
    };
    const auto event_completed = event_continuation.resume(
        event_continuation.state,
        event_context,
        event_packet
    );
    assert(event_completed.state == EScriptStepState::COMPLETED);
    assert(test::coroutine_event_value == event_value);
    event_continuation.destroy(event_continuation.state);
    descriptor.releaseMethod(descriptor.context, instance, event_method_prepared);

    ScriptBackendPreparedMethod multi_event_method;
    assert(descriptor.prepareMethod(
        descriptor.context,
        instance,
        entity_asset.description().exports[5],
        multi_event_method
    ) == EScriptBackendResult::SUCCESS);
    const auto multi_event_call = multi_event_method.resumable;
    ScriptBackendContinuation first_event_continuation;
    ScriptBackendContinuation second_event_continuation;
    ScriptBackendContinuation exhausted_event_continuation;
    const auto first_event_wait = multi_event_call.invoke(
        multi_event_call.context,
        empty_frame,
        event_context,
        first_event_continuation
    );
    const auto second_event_wait = multi_event_call.invoke(
        multi_event_call.context,
        empty_frame,
        event_context,
        second_event_continuation
    );
    const auto exhausted_event_wait = multi_event_call.invoke(
        multi_event_call.context,
        empty_frame,
        event_context,
        exhausted_event_continuation
    );
    assert(first_event_wait.state == EScriptStepState::SUSPENDED);
    assert(second_event_wait.state == EScriptStepState::SUSPENDED);
    assert(exhausted_event_wait.state == EScriptStepState::FAILED);
    assert(!exhausted_event_continuation);
    first_event_continuation.destroy(first_event_continuation.state);
    second_event_continuation.destroy(second_event_continuation.state);
    descriptor.releaseMethod(descriptor.context, instance, multi_event_method);

    ScriptBackendPreparedMethod delay_method;
    assert(descriptor.prepareMethod(
        descriptor.context,
        instance,
        entity_asset.description().exports[6],
        delay_method
    ) == EScriptBackendResult::SUCCESS);
    const auto delay_step_call = delay_method.resumable;
    ScriptBackendContinuation delay_continuation;
    const auto delay_suspended = delay_step_call.invoke(
        delay_step_call.context,
        empty_frame,
        step_context,
        delay_continuation
    );
    assert(delay_suspended.state == EScriptStepState::SUSPENDED);
    const ScriptResumePacket delay_packet{
        delay_suspended.waiting_on,
        EScriptAwaitableState::READY,
        nullptr,
        {}
    };
    const auto delay_completed = delay_continuation.resume(
        delay_continuation.state,
        step_context,
        delay_packet
    );
    assert(delay_completed.state == EScriptStepState::COMPLETED);
    assert(test::coroutine_value == 115);
    delay_continuation.destroy(delay_continuation.state);
    descriptor.releaseMethod(descriptor.context, instance, delay_method);

    ScriptBackendPreparedMethod twice_method_prepared;
    assert(descriptor.prepareMethod(
        descriptor.context,
        instance,
        entity_asset.description().exports[7],
        twice_method_prepared
    ) == EScriptBackendResult::SUCCESS);
    const auto twice_step_call = twice_method_prepared.resumable;
    ScriptBackendContinuation twice_continuation;
    const auto twice_first = twice_step_call.invoke(
        twice_step_call.context,
        empty_frame,
        step_context,
        twice_continuation
    );
    assert(twice_first.state == EScriptStepState::SUSPENDED);
    const ScriptResumePacket twice_first_packet{
        twice_first.waiting_on,
        EScriptAwaitableState::READY,
        nullptr,
        {}
    };
    const auto twice_second = twice_continuation.resume(
        twice_continuation.state,
        step_context,
        twice_first_packet
    );
    assert(twice_second.state == EScriptStepState::SUSPENDED);
    assert(test::coroutine_value == 1'115);
    const ScriptResumePacket twice_second_packet{
        twice_second.waiting_on,
        EScriptAwaitableState::READY,
        nullptr,
        {}
    };
    const auto twice_completed = twice_continuation.resume(
        twice_continuation.state,
        step_context,
        twice_second_packet
    );
    assert(twice_completed.state == EScriptStepState::COMPLETED);
    assert(test::coroutine_value == 11'115);
    twice_continuation.destroy(twice_continuation.state);
    descriptor.releaseMethod(descriptor.context, instance, twice_method_prepared);
    const auto coroutine_stats = backend.stats();
    assert(coroutine_stats.active_frames == 0U);
    assert(coroutine_stats.frame_high_water == 2U);
    assert(coroutine_stats.heap_frame_allocations == 0U);

    auto tampered_description = entity_asset.description();
    tampered_description.module_name = "lux.test.tampered";
    auto tampered = lux::script::ScriptArtifact::create(std::move(tampered_description), {});
    assert(tampered);
    ScriptBackendInstance rejected_instance;
    assert(descriptor.createInstance(
        descriptor.context,
        ScriptInstanceCreateContext{
            assetId(),
            EntityScriptScope{ecs::Entity{2U}},
            &behavior},
        *tampered,
        rejected_instance
    ) == EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH);

    descriptor.releaseMethod(descriptor.context, instance, end_method_prepared);
    descriptor.releaseMethod(descriptor.context, instance, begin_method_prepared);
    descriptor.releaseMethod(descriptor.context, instance, call_method);
    descriptor.destroyInstance(descriptor.context, instance);
    assert(test::constructed_objects == 1U);
    assert(test::destroyed_objects == 1U);
    ScriptBackendInstance recycled_instance;
    assert(descriptor.createInstance(
        descriptor.context,
        ScriptInstanceCreateContext{
            assetId(),
            EntityScriptScope{ecs::Entity{4U}},
            &behavior},
        entity_asset,
        recycled_instance
    ) == EScriptBackendResult::SUCCESS);
    assert(recycled_instance.value == instance.value);
    ScriptBackendPreparedMethod recycled_method;
    assert(descriptor.prepareMethod(
        descriptor.context,
        recycled_instance,
        entity_asset.description().exports[0],
        recycled_method
    ) == EScriptBackendResult::SUCCESS);
    const auto recycled_call = recycled_method.synchronous;
    assert(recycled_call.context == call.context);
    descriptor.releaseMethod(
        descriptor.context,
        recycled_instance,
        recycled_method
    );
    descriptor.destroyInstance(descriptor.context, recycled_instance);
    assert(test::constructed_objects == 2U);
    assert(test::destroyed_objects == 2U);

    auto global_asset_result = lux::script::ScriptArtifact::create(global->description(), {});
    assert(global_asset_result);
    auto global_asset = std::move(*global_asset_result);
    const std::array global_pools{CppStaticScriptPoolDescription{
        std::addressof(*global), 1U, 0U, 0U, alignof(std::max_align_t), 1U
    }};
    auto global_backend_result = CppStaticScriptBackend::create(global_pools);
    assert(global_backend_result);
    auto global_backend = std::move(*global_backend_result);
    const auto global_descriptor = global_backend.descriptor();
    ScriptBackendInstance global_instance;
    assert(global_descriptor.createInstance(
        global_descriptor.context,
        ScriptInstanceCreateContext{
            assetId(),
            SimulationScriptScope{},
            nullptr},
        global_asset,
        global_instance
    ) == EScriptBackendResult::SUCCESS);
    ScriptBackendPreparedMethod global_method;
    assert(global_descriptor.prepareMethod(
        global_descriptor.context,
        global_instance,
        global_asset.description().exports[0],
        global_method
    ) == EScriptBackendResult::SUCCESS);
    const auto global_call = global_method.synchronous;
    std::int32_t input{4};
    std::int32_t output{};
    lux_script_value_slot input_slot{
        LUX_SCRIPT_VK_INT32, {}, sizeof(input),
        lux::semantic::typeId("lux.i32"), &input};
    lux_script_value_slot output_slot{
        LUX_SCRIPT_VK_INT32, {}, sizeof(output),
        lux::semantic::typeId("lux.i32"), &output};
    lux_script_call_frame global_frame{
        &input_slot, 1U, 0U, &output_slot, 1U, 0U, nullptr,
        global_call.context};
    assert(global_call.invoke(&global_frame) == 0);
    assert(output == 5);
    global_descriptor.releaseMethod(
        global_descriptor.context,
        global_instance,
        global_method);
    global_descriptor.destroyInstance(
        global_descriptor.context,
        global_instance);

    lux::meta::RefClass ability_object_class;
    ability_object_class.name = "CoroutineAbilityObject";
    ability_object_class.full_name = "lux.test.CoroutineAbilityObject";
    ability_object_class.type = lux::meta::ref_type_of_v<CoroutineAbilityObject>;
    ability_object_class.construct = [](void* memory)
    {
        std::construct_at(static_cast<CoroutineAbilityObject*>(memory));
    };
    ability_object_class.destruct = [](void* object)
    {
        std::destroy_at(static_cast<CoroutineAbilityObject*>(object));
    };
    lux::meta::RefMethod ability_method;
    ability_method.owner_class = std::addressof(ability_object_class);
    ability_method.visibility = lux::meta::EVisibility::Public;
    ability_method.is_noexcept = true;
    ability_method.invokable.name = "execute";
    ability_method.invokable.full_name = "lux.test.CoroutineAbilityObject::execute";
    ability_method.invokable.return_type = lux::meta::ref_type_of_v<ScriptCoroutine>;
    ability_method.invokable.parameters.push_back({
        "context",
        lux::meta::ref_type_of_v<ScriptCoroutineContext&>,
        "lux::simulation::script::ScriptCoroutineContext",
        lux::cxx::type_hash<ScriptCoroutineContext>(),
        false
    });
    const auto ability_coroutine = makeCppStaticCoroutineExport<
        &CoroutineAbilityObject::execute
    >(ability_method, lux::script::ScriptSymbolId{301U});
    const std::array ability_coroutines{ability_coroutine};
    using CoroutineAbility = lux::simulation::test::CppCoroutineAbility;
    using CoroutineAbilityTraits = lux::script::ScriptAbilityTraits<CoroutineAbility>;
    const std::array coroutine_ability_requirements{
        lux::rdesc::ScriptApiRequirement{
            lux::script::ScriptApiContractId{CoroutineAbilityTraits::Description.id.name()},
            CoroutineAbilityTraits::Description.schema_hash
        }
    };
    const std::array<const lux::meta::RefMethod*, 0U> no_methods{};
    const std::array<lux::script::ScriptSymbolId, 0U> no_symbols{};
    auto ability_descriptor = projectCppStaticEntityScript(
        "lux.test.cpp-coroutine-ability",
        "cpp-coroutine-ability-v1",
        ability_object_class,
        no_methods,
        no_symbols,
        {},
        nullptr,
        {},
        ability_coroutines,
        coroutine_ability_requirements
    );
    assert(ability_descriptor);
    auto ability_artifact = lux::script::ScriptArtifact::create(ability_descriptor->description(), {});
    assert(ability_artifact);
    const std::array ability_pools{CppStaticScriptPoolDescription{
        std::addressof(*ability_descriptor),
        1U,
        1U,
        2048U,
        alignof(std::max_align_t),
        1U
    }};
    auto ability_backend_result = CppStaticScriptBackend::create(ability_pools);
    assert(ability_backend_result);
    auto ability_backend = std::move(*ability_backend_result);
    const auto ability_backend_descriptor = ability_backend.descriptor();
    CoroutineAbilityProvider coroutine_provider;
    const auto coroutine_binding = lux::script::bindScriptAbility<CoroutineAbility>(coroutine_provider);
    const std::array coroutine_capabilities{
        PreparedScriptApiCapability{
            lux::script::ScriptApiContractId{CoroutineAbilityTraits::Description.id.name()},
            CoroutineAbilityTraits::Description.schema_hash,
            coroutine_binding.context,
            coroutine_binding.dispatch,
            CoroutineAbilityTraits::Description.schema_version,
            coroutine_binding.erased_methods
        }
    };
    ScriptBackendInstance ability_instance;
    assert(ability_backend_descriptor.createInstance(
        ability_backend_descriptor.context,
        ScriptInstanceCreateContext{
            assetId(),
            EntityScriptScope{ecs::Entity{9U}},
            &behavior,
            {3U, 1U},
            coroutine_capabilities
        },
        *ability_artifact,
        ability_instance
    ) == EScriptBackendResult::SUCCESS);
    ScriptBackendPreparedMethod ability_method_prepared;
    assert(ability_backend_descriptor.prepareMethod(
        ability_backend_descriptor.context,
        ability_instance,
        ability_artifact->description().exports.front(),
        ability_method_prepared
    ) == EScriptBackendResult::SUCCESS);
    const auto ability_step = ability_method_prepared.resumable;
    ScriptStepContext ability_step_context{
        {3U, 1U},
        nullptr,
        &createAwaitable,
        &discardAwaitable
    };
    ScriptBackendContinuation ability_continuation;
    const auto ability_suspended = ability_step.invoke(
        ability_step.context,
        empty_frame,
        ability_step_context,
        ability_continuation
    );
    assert(ability_suspended.state == EScriptStepState::SUSPENDED);
    assert(generated_ability_value == 22);
    assert(coroutine_provider.value == 20);
    std::int32_t async_value{7};
    ScriptOwnedResumeValue owned_async;
    owned_async.type = lux::rdesc::makeScriptValueType<std::int32_t>(
        lux::semantic::EValuePass::VALUE
    );
    assert(owned_async.bytes.resize(sizeof(async_value), alignof(std::int32_t)));
    std::memcpy(owned_async.bytes.data(), std::addressof(async_value), sizeof(async_value));
    const ScriptResumePacket ability_packet{
        ability_suspended.waiting_on,
        EScriptAwaitableState::READY,
        std::addressof(owned_async),
        {}
    };
    const auto ability_completed = ability_continuation.resume(
        ability_continuation.state,
        ability_step_context,
        ability_packet
    );
    assert(ability_completed.state == EScriptStepState::COMPLETED);
    assert(generated_ability_value == 29);
    ability_continuation.destroy(ability_continuation.state);
    ScriptBackendContinuation failed_ability_continuation;
    const auto failed_ability_wait = ability_step.invoke(
        ability_step.context,
        empty_frame,
        ability_step_context,
        failed_ability_continuation
    );
    assert(failed_ability_wait.state == EScriptStepState::SUSPENDED);
    const ScriptResumePacket failed_ability_packet{
        failed_ability_wait.waiting_on,
        EScriptAwaitableState::FAILED,
        nullptr,
        {77}
    };
    const auto failed_ability = failed_ability_continuation.resume(
        failed_ability_continuation.state,
        ability_step_context,
        failed_ability_packet
    );
    assert(failed_ability.state == EScriptStepState::FAILED);
    assert(failed_ability.error.status == 77);
    failed_ability_continuation.destroy(failed_ability_continuation.state);
    coroutine_provider.reject = true;
    ScriptBackendContinuation rejected_ability_continuation;
    const auto rejected_ability = ability_step.invoke(
        ability_step.context,
        empty_frame,
        ability_step_context,
        rejected_ability_continuation
    );
    assert(rejected_ability.state == EScriptStepState::FAILED);
    assert(!rejected_ability_continuation);
    ability_backend_descriptor.releaseMethod(
        ability_backend_descriptor.context,
        ability_instance,
        ability_method_prepared
    );
    ability_backend_descriptor.destroyInstance(
        ability_backend_descriptor.context,
        ability_instance
    );
    assert(ability_backend.stats().active_frames == 0U);
    return 0;
}

#include <lux/engine/simulation/scripting/native/NativeScriptBackend.hpp>
#include <lux/engine/simulation/scripting/ScriptLifecycle.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace
{
    struct Provider final
    {
        struct Lease final
        {
            Provider* owner{};
            std::shared_ptr<lux::script::NativeModule> module;
        };

        std::array<std::shared_ptr<lux::script::NativeModule>, 2U> modules;
        std::size_t resolves{};
        std::size_t releases{};

        static void release(void* opaque) noexcept
        {
            auto* lease = static_cast<Lease*>(opaque);
            ++lease->owner->releases;
            delete lease;
        }

        static bool resolve(
            void* opaque,
            const lux::asset::AssetId& asset,
            const lux::script::ScriptArtifact&,
            lux::simulation::script::ResolvedNativeModule& result
        ) noexcept
        {
            auto& self = *static_cast<Provider*>(opaque);
            ++self.resolves;
            const auto bytes = asset.bytes();
            const auto index = std::to_integer<std::uint8_t>(bytes[0]) == 2U
                ? 1U
                : 0U;
            auto* lease = new (std::nothrow) Lease{
                &self,
                self.modules[index]};
            if (!lease)
                return false;
            result = {lease->module.get(), lease, &release};
            return true;
        }
    };

    struct AsyncProvider final
    {
        lux::script::ScriptAbilityErasedCompletion completion;

        static lux::script::ScriptAbilityStartResult start(
            void* opaque,
            const void*,
            std::span<const lux::script::ScriptAbilityInputSlot> arguments,
            lux::script::ScriptAbilityErasedCompletion completion
        ) noexcept
        {
            if (!arguments.empty())
                return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{81});
            static_cast<AsyncProvider*>(opaque)->completion = std::move(completion);
            return {};
        }
    };

    struct AwaitableHarness final
    {
        lux::simulation::script::ScriptInstanceId instance{1U, 1U};
        lux::simulation::script::ScriptAwaitableId awaiting{1U, 1U};
        std::shared_ptr<void> lease{std::make_shared<int>(0)};
        bool completed{};

        static lux::cxx::expected<lux::simulation::script::ScriptAwaitableRegistration,
                                  lux::simulation::script::EScriptAwaitableCreateError>
        create(
            void* opaque,
            lux::simulation::script::ScriptInstanceId instance,
            std::optional<lux::rdesc::ScriptValueType> result
        ) noexcept
        {
            auto& self = *static_cast<AwaitableHarness*>(opaque);
            if (instance != self.instance || result)
            {
                return lux::cxx::unexpected(
                    lux::simulation::script::EScriptAwaitableCreateError::INVALID_RESULT_TYPE
                );
            }
            return lux::simulation::script::ScriptAwaitableRegistration{
                self.awaiting,
                lux::simulation::script::ScriptAwaitableCompletion{
                    self.lease,
                    std::addressof(self),
                    &complete,
                    &active,
                    self.instance,
                    self.awaiting,
                    &abilitySuccess,
                    &abilityFailure,
                    &abilityActive
                }
            };
        }

        static void discard(
            void*,
            lux::simulation::script::ScriptInstanceId,
            lux::simulation::script::ScriptAwaitableId
        ) noexcept
        {
        }

        static lux::cxx::expected<void, lux::simulation::script::EScriptAwaitableCompletionError> complete(
            void* opaque,
            lux::simulation::script::ScriptInstanceId,
            lux::simulation::script::ScriptAwaitableId,
            lux::simulation::script::EScriptAwaitableState state,
            lux::simulation::script::ScriptOwnedResumeValue,
            lux::simulation::script::ScriptStepError
        ) noexcept
        {
            auto& self = *static_cast<AwaitableHarness*>(opaque);
            if (self.completed)
            {
                return lux::cxx::unexpected(
                    lux::simulation::script::EScriptAwaitableCompletionError::ALREADY_TERMINAL
                );
            }
            self.completed = state == lux::simulation::script::EScriptAwaitableState::READY;
            return {};
        }

        static bool active(
            void* opaque,
            lux::simulation::script::ScriptInstanceId,
            lux::simulation::script::ScriptAwaitableId
        ) noexcept
        {
            return !static_cast<AwaitableHarness*>(opaque)->completed;
        }

        static lux::cxx::expected<void, lux::script::EScriptAbilityCompletionError> abilitySuccess(
            void* opaque,
            std::uint64_t,
            std::uint64_t,
            lux::semantic::TypeId type,
            const void* data,
            std::uint32_t size
        ) noexcept
        {
            auto& self = *static_cast<AwaitableHarness*>(opaque);
            if (self.completed)
                return lux::cxx::unexpected(lux::script::EScriptAbilityCompletionError::ALREADY_COMPLETED);
            if (type != lux::semantic::InvalidTypeId || data != nullptr || size != 0U)
                return lux::cxx::unexpected(lux::script::EScriptAbilityCompletionError::INVALID_VALUE);
            self.completed = true;
            return {};
        }

        static lux::cxx::expected<void, lux::script::EScriptAbilityCompletionError> abilityFailure(
            void*,
            std::uint64_t,
            std::uint64_t,
            lux::script::ScriptAbilityOperationError
        ) noexcept
        {
            return {};
        }

        static bool abilityActive(void* opaque, std::uint64_t, std::uint64_t) noexcept
        {
            return !static_cast<AwaitableHarness*>(opaque)->completed;
        }
    };
}

int main()
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    auto loaded = lux::script::loadNativeModule(
        std::filesystem::path{LUX_SCRIPT_NATIVE_FIXTURE}
    );
    assert(loaded);
    auto module = std::make_shared<lux::script::NativeModule>(
        std::move(*loaded)
    );
    auto loaded_second = lux::script::loadNativeModule(
        std::filesystem::path{LUX_SCRIPT_NATIVE_SECOND_FIXTURE}
    );
    assert(loaded_second);
    auto second_module = std::make_shared<lux::script::NativeModule>(
        std::move(*loaded_second)
    );
    Provider provider{{module, second_module}};
    auto backend = std::make_unique<NativeScriptBackend>(
        NativeModuleResolver{&provider, &Provider::resolve},
        NativeScriptBackendConfig{
            .module_capacity = 2U,
            .instance_capacity = 3U,
            .prepared_call_capacity = 8U,
            .continuation_capacity = 4U,
            .max_ability_imports_per_module = 8U,
            .max_continuation_frame_bytes = 4096U,
            .continuation_frame_storage_bytes = 16384U
        }
    );
    assert(*backend);
    assert(backend->stats().frame_storage_bytes == 16384U);
    assert(backend->stats().heap_frame_allocations == 0U);

    lux::rdesc::Script description;
    description.module_name = "native_fixture";
    description.body = lux::rdesc::NativeModuleScript{
        LUX_SCRIPT_ABI_VERSION,
        module->stateLayoutHash(),
        64U,
        64U,
        {}};
    const lux::rdesc::ScriptFunction increment{
        "Increment",
        1U,
        {},
        {}};
    const lux::rdesc::ScriptFunction function{
        "OnUpdate",
        2U,
        {lux::rdesc::makeScriptValueType<float>()},
        {}};
    const lux::rdesc::ScriptFunction pair{
        "OnUpdate",
        3U,
        {
            lux::rdesc::makeScriptValueType<float>(),
            lux::rdesc::makeScriptValueType<std::uint32_t>()},
        {}};
    const lux::rdesc::ScriptFunction begin_lifecycle{
        "AdmitToGameplay",
        4U,
        {},
        {}};
    const lux::rdesc::ScriptFunction end_lifecycle{
        "LeaveGameplay",
        5U,
        {lux::rdesc::makeScriptValueType<EScriptEndPlayReason>()},
        {}};
    description.exports = {increment, function, pair, begin_lifecycle, end_lifecycle};
    description.lifecycle = {begin_lifecycle.symbol_id, end_lifecycle.symbol_id};
    auto asset_result = lux::script::ScriptArtifact::create(description, {});
    assert(asset_result);
    auto asset = std::move(*asset_result);

    auto second_description = description;
    second_description.module_name = "native_fixture_two";
    auto& second_body = std::get<lux::rdesc::NativeModuleScript>(
        second_description.body
    );
    second_body.state_layout_hash = second_module->stateLayoutHash();
    auto second_asset_result = lux::script::ScriptArtifact::create(std::move(second_description), {});
    assert(second_asset_result);
    auto second_asset = std::move(*second_asset_result);

    auto descriptor = backend->descriptor();
    std::array<std::uint8_t, 16U> id_bytes{};
    id_bytes[0] = 1U;
    const auto expect_contract_mismatch = [&](const auto& content)
    {
        ScriptBackendInstance rejected_instance;
        assert(descriptor.createInstance(
            descriptor.context,
            ScriptInstanceCreateContext{
                lux::asset::AssetId{id_bytes},
                SimulationScriptScope{},
                nullptr},
            content,
            rejected_instance
        ) == EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH);
        assert(!rejected_instance);
    };
    {
        auto tampered_description = description;
        tampered_description.module_name = "wrong_native_module";
        auto tampered = lux::script::ScriptArtifact::create(std::move(tampered_description), {});
        assert(tampered);
        expect_contract_mismatch(*tampered);
    }
    {
        auto tampered_description = description;
        auto& body = std::get<lux::rdesc::NativeModuleScript>(
            tampered_description.body
        );
        body.abi_version = 1U;
        auto tampered = lux::script::ScriptArtifact::create(std::move(tampered_description), {});
        assert(tampered);
        expect_contract_mismatch(*tampered);
    }
    {
        auto tampered_description = description;
        auto& body = std::get<lux::rdesc::NativeModuleScript>(
            tampered_description.body
        );
        ++body.state_layout_hash;
        auto tampered = lux::script::ScriptArtifact::create(std::move(tampered_description), {});
        assert(tampered);
        expect_contract_mismatch(*tampered);
    }
    {
        auto tampered_description = description;
        tampered_description.exports[1].args[0].pass =
            lux::semantic::EValuePass::CONST_REF;
        auto tampered = lux::script::ScriptArtifact::create(std::move(tampered_description), {});
        assert(tampered);
        expect_contract_mismatch(*tampered);
    }
    ScriptBackendInstance first_instance;
    ScriptBackendInstance second_instance;
    ScriptBackendInstance over_capacity;
    assert(descriptor.createInstance(
        descriptor.context,
        ScriptInstanceCreateContext{
            lux::asset::AssetId{id_bytes},
            SimulationScriptScope{},
            nullptr},
        asset,
        first_instance
    ) == EScriptBackendResult::SUCCESS);
    assert(descriptor.createInstance(
        descriptor.context,
        ScriptInstanceCreateContext{
            lux::asset::AssetId{id_bytes},
            SimulationScriptScope{},
            nullptr},
        asset,
        second_instance
    ) == EScriptBackendResult::SUCCESS);
    id_bytes[0] = 2U;
    ScriptBackendInstance third_instance;
    assert(descriptor.createInstance(
        descriptor.context,
        ScriptInstanceCreateContext{
            lux::asset::AssetId{id_bytes},
            SimulationScriptScope{},
            nullptr},
        second_asset,
        third_instance
    ) == EScriptBackendResult::SUCCESS);
    id_bytes[0] = 3U;
    assert(descriptor.createInstance(
        descriptor.context,
        ScriptInstanceCreateContext{
            lux::asset::AssetId{id_bytes},
            SimulationScriptScope{},
            nullptr},
        asset,
        over_capacity
    ) == EScriptBackendResult::CAPACITY_EXCEEDED);

    lux::script::BoundScriptCall first;
    lux::script::BoundScriptCall second;
    lux::script::BoundScriptCall third;
    assert(descriptor.prepareMethod(
        descriptor.context,
        first_instance,
        function,
        first
    ) == EScriptBackendResult::SUCCESS);
    assert(descriptor.prepareMethod(
        descriptor.context,
        third_instance,
        function,
        third
    ) == EScriptBackendResult::SUCCESS);
    assert(descriptor.prepareMethod(
        descriptor.context,
        second_instance,
        function,
        second
    ) == EScriptBackendResult::SUCCESS);
    assert(first && second && third && first.context != second.context &&
        second.context != third.context);

    float delta = 2.5F;
    lux_script_value_slot argument{
        LUX_SCRIPT_VK_FLOAT,
        {},
        sizeof(delta),
        lux::semantic::typeId("lux.f32"),
        &delta};
    lux_script_call_frame frame{
        &argument, 1U, 0U, nullptr, 0U, 0U, nullptr, first.context};
    assert(first.invoke(&frame) == 0);
    frame.user_context = second.context;
    assert(second.invoke(&frame) == 0);
    frame.user_context = third.context;
    assert(third.invoke(&frame) == 0);

    lux::script::BoundScriptCall begin_call;
    lux::script::BoundScriptCall end_call;
    assert(descriptor.prepareMethod(
        descriptor.context,
        first_instance,
        begin_lifecycle,
        begin_call
    ) == EScriptBackendResult::SUCCESS);
    assert(descriptor.prepareMethod(
        descriptor.context,
        first_instance,
        end_lifecycle,
        end_call
    ) == EScriptBackendResult::SUCCESS);
    lux_script_call_frame begin_frame{
        nullptr, 0U, 0U, nullptr, 0U, 0U, nullptr, begin_call.context};
    assert(begin_call.invoke(&begin_frame) == 0);
    const EScriptEndPlayReason end_reason{EScriptEndPlayReason::RUNTIME_STOPPED};
    lux_script_value_slot end_slot{
        LUX_SCRIPT_VK_UINT32,
        {},
        sizeof(end_reason),
        lux::semantic::typeId("lux.simulation.ScriptEndPlayReason"),
        const_cast<EScriptEndPlayReason*>(std::addressof(end_reason))};
    lux_script_call_frame end_frame{
        &end_slot, 1U, 0U, nullptr, 0U, 0U, nullptr, end_call.context};
    assert(end_call.invoke(&end_frame) == 0);

    auto mismatched = function;
    mismatched.args[0].canonical_name = "lux.f64";
    mismatched.args[0].type_id = lux::semantic::typeId("lux.f64");
    lux::script::BoundScriptCall rejected;
    assert(descriptor.prepareMethod(
        descriptor.context,
        first_instance,
        mismatched,
        rejected
    ) == EScriptBackendResult::UNSUPPORTED_SIGNATURE);

    descriptor.releaseMethod(descriptor.context, first_instance, first);
    descriptor.releaseMethod(descriptor.context, first_instance, end_call);
    descriptor.releaseMethod(descriptor.context, first_instance, begin_call);
    descriptor.releaseMethod(descriptor.context, second_instance, second);
    descriptor.releaseMethod(descriptor.context, third_instance, third);
    descriptor.destroyInstance(descriptor.context, first_instance);
    descriptor.destroyInstance(descriptor.context, second_instance);
    descriptor.destroyInstance(descriptor.context, third_instance);
    id_bytes[0] = 1U;
    ScriptBackendInstance recycled_instance;
    assert(descriptor.createInstance(
        descriptor.context,
        ScriptInstanceCreateContext{
            lux::asset::AssetId{id_bytes},
            SimulationScriptScope{},
            nullptr},
        asset,
        recycled_instance
    ) == EScriptBackendResult::SUCCESS);
    assert(recycled_instance.value == third_instance.value);
    lux::script::BoundScriptCall recycled_call;
    assert(descriptor.prepareMethod(
        descriptor.context,
        recycled_instance,
        function,
        recycled_call
    ) == EScriptBackendResult::SUCCESS);
    assert(recycled_call);
    descriptor.releaseMethod(
        descriptor.context,
        recycled_instance,
        recycled_call
    );
    descriptor.destroyInstance(descriptor.context, recycled_instance);
    assert(provider.resolves == 6U);
    backend.reset();
    module.reset();
    second_module.reset();
    assert(provider.releases == 6U);

    auto loaded_step = lux::script::loadNativeModule(std::filesystem::path{LUX_SCRIPT_NATIVE_STEP_FIXTURE});
    assert(loaded_step);
    auto step_module = std::make_shared<lux::script::NativeModule>(std::move(*loaded_step));
    Provider step_modules{{step_module, step_module}};
    NativeScriptBackend step_backend{
        NativeModuleResolver{&step_modules, &Provider::resolve},
        NativeScriptBackendConfig{
            .module_capacity = 1U,
            .instance_capacity = 1U,
            .prepared_call_capacity = 4U,
            .continuation_capacity = 2U,
            .max_ability_imports_per_module = 2U,
            .max_continuation_frame_bytes = 256U,
            .continuation_frame_storage_bytes = 512U
        }
    };
    assert(step_backend);

    lux::rdesc::Script step_description;
    step_description.module_name = "native_step_fixture";
    step_description.body = lux::rdesc::NativeModuleScript{
        LUX_SCRIPT_ABI_VERSION,
        step_module->stateLayoutHash(),
        step_module->stateSize(),
        step_module->stateAlignment(),
        {}
    };
    const lux::rdesc::ScriptFunction async_update{"AsyncUpdate", 6U, {}, {}};
    const lux::rdesc::ScriptFunction read_state{
        "ReadState",
        7U,
        {},
        {lux::rdesc::makeScriptValueType<std::uint32_t>()}
    };
    step_description.exports = {async_update, read_state};
    step_description.api_requirements.push_back({
        lux::script::ScriptApiContractId{"lux.test.native_async"},
        0xA55A55A5ULL
    });
    auto step_artifact_result = lux::script::ScriptArtifact::create(std::move(step_description), {});
    assert(step_artifact_result);

    AsyncProvider async_provider;
    static constexpr std::array async_methods{
        lux::script::ScriptAbilityErasedMethodBinding{
            lux::script::ScriptApiMethodIdView{"lux.test.native_async.wait"},
            lux::script::EScriptApiMethodKind::ASYNC_OPERATION,
            {},
            {},
            nullptr,
            &AsyncProvider::start
        }
    };
    const std::array capabilities{
        PreparedScriptApiCapability{
            lux::script::ScriptApiContractId{"lux.test.native_async"},
            0xA55A55A5ULL,
            std::addressof(async_provider),
            std::addressof(async_provider),
            1U,
            async_methods
        }
    };
    auto step_descriptor = step_backend.descriptor();
    ScriptBackendInstance step_instance;
    std::array<std::uint8_t, 16U> step_id_bytes{};
    step_id_bytes[0] = 4U;
    assert(step_descriptor.createInstance(
        step_descriptor.context,
        ScriptInstanceCreateContext{
            lux::asset::AssetId{step_id_bytes},
            SimulationScriptScope{},
            nullptr,
            {1U, 1U},
            capabilities
        },
        *step_artifact_result,
        step_instance
    ) == EScriptBackendResult::SUCCESS);

    lux::script::BoundScriptCall read_call;
    BoundScriptStepCall step_call;
    assert(step_descriptor.prepareMethod(
        step_descriptor.context,
        step_instance,
        read_state,
        read_call
    ) == EScriptBackendResult::SUCCESS);
    assert(step_descriptor.prepareStepMethod(
        step_descriptor.context,
        step_instance,
        async_update,
        step_call
    ) == EScriptBackendResult::SUCCESS);

    AwaitableHarness awaitable;
    ScriptStepContext step_context{
        awaitable.instance,
        std::addressof(awaitable),
        &AwaitableHarness::create,
        &AwaitableHarness::discard
    };
    lux_script_call_frame step_frame{};
    ScriptBackendContinuation continuation;
    const auto suspended = step_call.invoke(step_call.context, step_frame, step_context, continuation);
    assert(suspended.state == EScriptStepState::SUSPENDED);
    assert(suspended.waiting_on == awaitable.awaiting);
    assert(continuation);
    assert(step_backend.stats().active_frames == 1U);
    assert(step_backend.stats().frame_high_water == 1U);
    assert(step_backend.stats().heap_frame_allocations == 0U);
    assert(async_provider.completion);
    assert(async_provider.completion.success());
    assert(awaitable.completed);
    const ScriptOwnedResumeValue no_value;
    const ScriptResumePacket resume_packet{
        awaitable.awaiting,
        EScriptAwaitableState::READY,
        std::addressof(no_value),
        {}
    };
    const auto resumed = continuation.resume(continuation.state, step_context, resume_packet);
    assert(resumed.state == EScriptStepState::COMPLETED);
    continuation.destroy(continuation.state);
    assert(step_backend.stats().active_frames == 0U);

    std::uint32_t state_value{};
    lux_script_value_slot state_result{
        LUX_SCRIPT_VK_UINT32,
        {},
        sizeof(state_value),
        lux::semantic::typeId("lux.u32"),
        std::addressof(state_value)
    };
    lux_script_call_frame read_frame{};
    read_frame.returns = std::addressof(state_result);
    read_frame.return_count = 1U;
    read_frame.user_context = read_call.context;
    assert(read_call.invoke(std::addressof(read_frame)) == 0);
    assert(state_value == 1U);

    step_descriptor.releaseStepMethod(step_descriptor.context, step_instance, step_call);
    step_descriptor.releaseMethod(step_descriptor.context, step_instance, read_call);
    step_descriptor.destroyInstance(step_descriptor.context, step_instance);
}

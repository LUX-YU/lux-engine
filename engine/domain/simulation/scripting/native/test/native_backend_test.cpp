#include <lux/engine/simulation/scripting/native/NativeScriptBackend.hpp>
#include <lux/engine/simulation/scripting/ScriptLifecycle.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

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
        2U,
        3U);
    assert(*backend);

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
    assert(reinterpret_cast<std::uintptr_t>(first.context) % 64U == 0U);
    assert(reinterpret_cast<std::uintptr_t>(second.context) % 64U == 0U);
    assert(reinterpret_cast<std::uintptr_t>(third.context) % 64U == 0U);

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
    assert(*static_cast<float*>(first.context) == delta);
    frame.user_context = second.context;
    assert(second.invoke(&frame) == 0);
    assert(*static_cast<float*>(second.context) == delta);
    frame.user_context = third.context;
    assert(third.invoke(&frame) == 0);
    assert(*static_cast<float*>(third.context) == delta);

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
    assert(*static_cast<float*>(first.context) == delta + 10.0F);
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
    assert(
        recycled_call.context == second.context ||
        recycled_call.context == first.context
    );
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
}

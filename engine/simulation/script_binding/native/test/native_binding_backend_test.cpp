#include <lux/engine/simulation/NativeScriptBindingBackend.hpp>

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
            const lux::asset::ScriptAssetContent&,
            lux::simulation::ResolvedNativeModule& result
        ) noexcept
        {
            auto& self = *static_cast<Provider*>(opaque);
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
    auto backend = std::make_unique<NativeScriptBindingBackend>(
        NativeModuleResolver{&provider, &Provider::resolve},
        2U,
        3U);
    assert(*backend);

    lux::asset::ScriptAssetContent asset;
    asset.description.module_name = "native_fixture";
    asset.description.body = lux::rdesc::NativeModuleScript{
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
    asset.description.exports = {increment, function, pair};
    assert(lux::rdesc::validScriptDescription(asset.description));

    auto second_asset = asset;
    second_asset.description.module_name = "native_fixture_two";
    auto& second_body = std::get<lux::rdesc::NativeModuleScript>(
        second_asset.description.body
    );
    second_body.state_layout_hash = second_module->stateLayoutHash();
    assert(lux::rdesc::validScriptDescription(second_asset.description));

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
                ScriptMountId{99U}},
            content,
            rejected_instance
        ) == EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH);
        assert(!rejected_instance);
    };
    {
        auto tampered = asset;
        tampered.description.module_name = "wrong_native_module";
        expect_contract_mismatch(tampered);
    }
    {
        auto tampered = asset;
        auto& body = std::get<lux::rdesc::NativeModuleScript>(
            tampered.description.body
        );
        body.abi_version = 1U;
        expect_contract_mismatch(tampered);
    }
    {
        auto tampered = asset;
        auto& body = std::get<lux::rdesc::NativeModuleScript>(
            tampered.description.body
        );
        ++body.state_layout_hash;
        expect_contract_mismatch(tampered);
    }
    {
        auto tampered = asset;
        tampered.description.exports[1].args[0].pass =
            lux::script::EScriptPassMode::CONST_REF;
        expect_contract_mismatch(tampered);
    }
    ScriptBackendInstance first_instance;
    ScriptBackendInstance second_instance;
    ScriptBackendInstance over_capacity;
    assert(descriptor.createInstance(
        descriptor.context,
        ScriptInstanceCreateContext{
            lux::asset::AssetId{id_bytes},
            ScriptMountId{1U}},
        asset,
        first_instance
    ) == EScriptBackendResult::SUCCESS);
    assert(descriptor.createInstance(
        descriptor.context,
        ScriptInstanceCreateContext{
            lux::asset::AssetId{id_bytes},
            ScriptMountId{2U}},
        asset,
        second_instance
    ) == EScriptBackendResult::SUCCESS);
    id_bytes[0] = 2U;
    ScriptBackendInstance third_instance;
    assert(descriptor.createInstance(
        descriptor.context,
        ScriptInstanceCreateContext{
            lux::asset::AssetId{id_bytes},
            ScriptMountId{3U}},
        second_asset,
        third_instance
    ) == EScriptBackendResult::SUCCESS);
    id_bytes[0] = 3U;
    assert(descriptor.createInstance(
        descriptor.context,
        ScriptInstanceCreateContext{
            lux::asset::AssetId{id_bytes},
            ScriptMountId{4U}},
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
        lux::script::scriptSemanticTypeId("lux.f32"),
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

    auto mismatched = function;
    mismatched.args[0].canonical_name = "lux.f64";
    mismatched.args[0].type_id = lux::script::scriptSemanticTypeId("lux.f64");
    lux::script::BoundScriptCall rejected;
    assert(descriptor.prepareMethod(
        descriptor.context,
        first_instance,
        mismatched,
        rejected
    ) == EScriptBackendResult::UNSUPPORTED_SIGNATURE);

    descriptor.releaseMethod(descriptor.context, first_instance, first);
    descriptor.releaseMethod(descriptor.context, second_instance, second);
    descriptor.releaseMethod(descriptor.context, third_instance, third);
    descriptor.destroyInstance(descriptor.context, first_instance);
    descriptor.destroyInstance(descriptor.context, second_instance);
    descriptor.destroyInstance(descriptor.context, third_instance);
    backend.reset();
    module.reset();
    second_module.reset();
    assert(provider.releases == 6U);
}

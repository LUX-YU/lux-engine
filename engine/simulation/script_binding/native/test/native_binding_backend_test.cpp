#include <lux/engine/simulation/NativeScriptBindingBackend.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

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
    NativeScriptBindingBackend backend{module, 2U};
    assert(backend);

    lux::asset::ScriptAssetContent asset;
    asset.description.module_name = "native_fixture";
    asset.description.model = lux::rdesc::EScriptModel::GLOBAL_MODULE;
    asset.description.body = lux::rdesc::NativeModuleScript{
        LUX_SCRIPT_ABI_VERSION,
        0U,
        64U,
        64U,
        {}};
    const lux::rdesc::ScriptFunction function{
        "OnUpdate",
        2U,
        {{"lux.f32", lux::script::scriptSemanticTypeId("lux.f32"),
          lux::script::EScriptPassMode::VALUE}},
        {}};
    asset.description.exports.push_back(function);
    assert(lux::rdesc::validScriptDescription(asset.description));

    auto descriptor = backend.descriptor();
    std::array<std::uint8_t, 16U> id_bytes{};
    id_bytes[0] = 1U;
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
    id_bytes[0] = 2U;
    assert(descriptor.createInstance(
        descriptor.context,
        ScriptInstanceCreateContext{
            lux::asset::AssetId{id_bytes},
            ScriptMountId{2U}},
        asset,
        second_instance
    ) == EScriptBackendResult::SUCCESS);
    id_bytes[0] = 3U;
    assert(descriptor.createInstance(
        descriptor.context,
        ScriptInstanceCreateContext{
            lux::asset::AssetId{id_bytes},
            ScriptMountId{3U}},
        asset,
        over_capacity
    ) == EScriptBackendResult::CAPACITY_EXCEEDED);

    lux::script::BoundScriptCall first;
    lux::script::BoundScriptCall second;
    assert(descriptor.prepareMethod(
        descriptor.context,
        first_instance,
        function,
        first
    ) == EScriptBackendResult::SUCCESS);
    assert(descriptor.prepareMethod(
        descriptor.context,
        second_instance,
        function,
        second
    ) == EScriptBackendResult::SUCCESS);
    assert(first && second && first.context != second.context);
    assert(reinterpret_cast<std::uintptr_t>(first.context) % 64U == 0U);
    assert(reinterpret_cast<std::uintptr_t>(second.context) % 64U == 0U);

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
    descriptor.destroyInstance(descriptor.context, first_instance);
    descriptor.destroyInstance(descriptor.context, second_instance);
    module.reset();
}

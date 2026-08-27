#include <lux/engine/simulation/NativeScriptBindingBackend.hpp>

#include <cassert>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

int main()
{
    auto loaded = lux::script::loadNativeModule(
        std::filesystem::path{LUX_SCRIPT_NATIVE_FIXTURE}
    );
    assert(loaded);
    auto module = std::make_shared<lux::script::NativeModule>(
        std::move(*loaded)
    );
    lux::simulation::NativeScriptBindingBackend backend{module};
    assert(backend);

    lux::asset::ScriptAssetContent asset;
    asset.description.schema_version = lux::rdesc::Script::kSchemaVersion;
    asset.description.module_name = "native_fixture";
    asset.description.body = lux::rdesc::NativeModuleScript{
        LUX_SCRIPT_ABI_VERSION,
        0U,
        sizeof(float),
        {}};
    lux::rdesc::ScriptFunction function{
        "OnUpdate",
        2U,
        {{"f32", lux::script::scriptSemanticTypeId("f32"),
          lux::script::EScriptPassMode::VALUE}},
        {}};
    asset.description.exports.push_back(function);

    auto descriptor = backend.descriptor();
    lux::script::BoundScriptCall first;
    lux::script::BoundScriptCall second;
    std::array<std::uint8_t, 16U> id_bytes{};
    id_bytes[0] = 1U;
    const lux::simulation::ScriptPrepareContext first_instance{
        lux::asset::AssetId{id_bytes},
        lux::simulation::ecs::NullEntity,
        0U,
        0U};
    id_bytes[0] = 2U;
    const lux::simulation::ScriptPrepareContext second_instance{
        lux::asset::AssetId{id_bytes},
        lux::simulation::ecs::NullEntity,
        1U,
        0U};
    assert(descriptor.prepare(
        descriptor.context,
        first_instance,
        asset,
        function,
        first
    ));
    assert(descriptor.prepare(
        descriptor.context,
        second_instance,
        asset,
        function,
        second
    ));
    assert(first && second && first.context != second.context);

    float delta = 2.5F;
    lux_script_value_slot argument{
        LUX_SCRIPT_VK_FLOAT,
        {},
        sizeof(delta),
        lux::script::scriptSemanticTypeId("f32"),
        &delta};
    lux_script_call_frame frame{
        &argument, 1U, 0U, nullptr, 0U, 0U, nullptr, first.context};
    assert(first.invoke(&frame) == 0);
    frame.user_context = first.context;
    assert(first.invoke(&frame) == 0);
    frame.user_context = second.context;
    assert(second.invoke(&frame) == 0);

    descriptor.release(descriptor.context, first);
    descriptor.release(descriptor.context, second);
    module.reset();
    // The prepared calls retained the module through their entire lifetime;
    // after release no hidden global lifetime remains.
    return 0;
}

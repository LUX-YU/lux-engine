#include "CppStaticScriptBridgeFixture.hpp"

#include <lux/engine/simulation/scripting/cpp_static/CppStaticScriptBridge.hpp>

#include <array>
#include <cassert>
#include <cstdint>

namespace
{
    using namespace lux::simulation::script;

    bool resolveRecord(
        void*,
        const lux::meta::RefType& type,
        lux::semantic::Layout& result
    ) noexcept
    {
        const auto* reflected = static_cast<const lux::meta::RefClass*>(
            type.ptr);
        if (!reflected || reflected->full_name !=
            "lux::simulation::test::BridgeRecord")
            return false;
        constexpr std::string_view name{"lux.test.BridgeRecord"};
        result = {
            lux::semantic::typeId(name),
            name,
            LUX_SCRIPT_VK_STRUCT_REF,
            sizeof(lux::simulation::test::BridgeRecord),
            alignof(lux::simulation::test::BridgeRecord)};
        return true;
    }

    [[nodiscard]] lux::asset::AssetId assetId()
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[0] = 0xA5U;
        return lux::asset::AssetId{bytes};
    }
}
int main()
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;
    namespace test = lux::simulation::test;

    lux::meta::ReflectionRegistry::initRegistry();
    auto& registry = lux::meta::ReflectionRegistry::instance();
    const auto* reflected = registry.findClass(
        "lux::simulation::test::BridgeBehavior");
    assert(reflected && reflected->methods.size() == 3U);

    const lux::meta::RefMethod* value_method{};
    const lux::meta::RefMethod* record_method{};
    const lux::meta::RefMethod* throwing_method{};
    for (const auto& method : reflected->methods)
    {
        if (method.invokable.name == "onValue")
            value_method = &method;
        else if (method.invokable.name == "onRecord")
            record_method = &method;
        else if (method.invokable.name == "throwing")
            throwing_method = &method;
        assert(method.invokable.name != "unmarkedHelper");
    }
    assert(value_method && record_method && throwing_method);

    const std::array selected{value_method, record_method};
    const std::array symbols{
        lux::script::ScriptSymbolId{101U},
        lux::script::ScriptSymbolId{102U}};
    auto projected = projectCppStaticEntityScript(
        "lux.test.bridge-behavior",
        "bridge-behavior-v1",
        *reflected,
        selected,
        symbols,
        CppStaticRecordSemanticResolver{nullptr, &resolveRecord}
    );
    assert(projected);
    assert(projected->description().schema_version == 7U);
    assert(projected->description().exports.size() == 2U);
    assert(projected->description().exports[0].args[0].canonical_name ==
        "lux.f32");
    assert(projected->description().exports[1].args[0].canonical_name ==
        "lux.test.BridgeRecord");
    assert(projected->description().exports[1].args[0].pass ==
        lux::semantic::EValuePass::CONST_REF);

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
    const std::array pools{CppStaticScriptPoolDescription{std::addressof(*projected), 1U}};
    const std::array duplicate_pools{
        CppStaticScriptPoolDescription{std::addressof(*projected), 1U},
        CppStaticScriptPoolDescription{std::addressof(*projected), 1U}
    };
    auto duplicate_backend = CppStaticScriptBackend::create(duplicate_pools);
    assert(!duplicate_backend);
    auto backend_result = CppStaticScriptBackend::create(pools);
    assert(backend_result);
    auto backend = std::move(*backend_result);
    const auto descriptor = backend.descriptor();
    ScriptBehavior behavior;
    ScriptBackendInstance instance;
    assert(descriptor.createInstance(
        descriptor.context,
        ScriptInstanceCreateContext{
            assetId(),
            EntityScriptScope{ecs::Entity{1U}},
            &behavior},
        entity_asset,
        instance
    ) == EScriptBackendResult::SUCCESS);

    lux::script::BoundScriptCall call;
    assert(descriptor.prepareMethod(
        descriptor.context,
        instance,
        entity_asset.description().exports[0],
        call
    ) == EScriptBackendResult::SUCCESS);
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

    descriptor.releaseMethod(descriptor.context, instance, call);
    descriptor.destroyInstance(descriptor.context, instance);
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
    lux::script::BoundScriptCall recycled_call;
    assert(descriptor.prepareMethod(
        descriptor.context,
        recycled_instance,
        entity_asset.description().exports[0],
        recycled_call
    ) == EScriptBackendResult::SUCCESS);
    assert(recycled_call.context == call.context);
    descriptor.releaseMethod(
        descriptor.context,
        recycled_instance,
        recycled_call
    );
    descriptor.destroyInstance(descriptor.context, recycled_instance);

    auto global_asset_result = lux::script::ScriptArtifact::create(global->description(), {});
    assert(global_asset_result);
    auto global_asset = std::move(*global_asset_result);
    const std::array global_pools{CppStaticScriptPoolDescription{std::addressof(*global), 1U}};
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
    lux::script::BoundScriptCall global_call;
    assert(global_descriptor.prepareMethod(
        global_descriptor.context,
        global_instance,
        global_asset.description().exports[0],
        global_call
    ) == EScriptBackendResult::SUCCESS);
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
        global_call);
    global_descriptor.destroyInstance(
        global_descriptor.context,
        global_instance);
    return 0;
}

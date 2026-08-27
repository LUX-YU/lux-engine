#include "CppStaticScriptBridgeFixture.hpp"

#include <lux/engine/simulation/CppStaticScriptBridge.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>

#include <array>
#include <cassert>
#include <cstdint>

namespace
{
    using namespace lux::simulation;

    inline constexpr std::array kBridgeHooks{makeSystemHookPoint<void(float)>("value", ESystemHookCardinality::MULTI)};
    inline constexpr SystemDescription kBridgeSystem{
        .canonical_name = "lux.test.bridge-system",
        .version = 1U,
        .hooks = kBridgeHooks};

    bool resolveRecord(void*, const lux::meta::RefType& type, lux::script::ScriptSemanticLayout& result) noexcept
    {
        const auto* reflected = static_cast<const lux::meta::RefClass*>(type.ptr);
        if (!reflected || reflected->full_name != "lux::simulation::test::BridgeRecord")
            return false;
        constexpr std::string_view name{"lux.test.BridgeRecord"};
        result = lux::script::ScriptSemanticLayout{
            lux::script::scriptSemanticTypeId(name),
            name,
            LUX_SCRIPT_VK_STRUCT_REF,
            sizeof(test::BridgeRecord),
            alignof(test::BridgeRecord)};
        return true;
    }

    struct Asset final
    {
        lux::asset::AssetId id;
        lux::asset::ScriptAssetContent content;
    };

    bool resolveAsset(void* opaque, const lux::asset::AssetId& id, ResolvedScriptAsset& result) noexcept
    {
        auto& asset = *static_cast<Asset*>(opaque);
        if (asset.id != id)
            return false;
        result.asset = &asset.content;
        return true;
    }
}

int
main()
{
    using namespace lux::simulation;
    lux::meta::ReflectionRegistry::initRegistry();
    auto& registry_meta = lux::meta::ReflectionRegistry::instance();
    const auto* reflected = registry_meta.findClass("lux::simulation::test::BridgeBehavior");
    assert(reflected);
    assert(reflected->methods.size() == 3U);

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
    auto projected = projectCppStaticEntityScript<test::BridgeBehavior>(
        "lux.test.bridge-behavior",
        "bridge-behavior-v1",
        *reflected,
        selected,
        CppStaticRecordSemanticResolver{nullptr, &resolveRecord}
    );
    assert(projected);
    assert(projected->description().schema_version == 4U);
    assert(projected->description().model == lux::rdesc::EScriptModel::ENTITY_BEHAVIOR);
    assert(projected->description().exports.size() == 2U);
    assert(projected->description().exports[0].args[0].canonical_name == "lux.f32");
    assert(projected->description().exports[1].args[0].canonical_name == "lux.test.BridgeRecord");
    assert(projected->description().exports[1].args[0].pass == lux::script::EScriptPassMode::CONST_REF);

    const std::array throwing{throwing_method};
    const auto rejected =
        projectCppStaticEntityScript<test::BridgeBehavior>("lux.test.throwing", "throwing-v1", *reflected, throwing);
    assert(!rejected);
    assert(rejected.error() == ECppStaticScriptBridgeError::METHOD_NOT_NOEXCEPT);

    const std::array parameter_ids{lux::cxx::type_hash<std::int32_t>()};
    const auto* reflected_function =
        registry_meta.findFunction("lux::simulation::test::bridgeFreeFunction", parameter_ids);
    assert(reflected_function && reflected_function->is_noexcept);
    const std::array functions{reflected_function};
    const auto global_projection = projectCppStaticGlobalScript("lux.test.bridge-free", "bridge-free-v1", functions);
    assert(global_projection);
    assert(global_projection->description().exports[0].args[0].canonical_name == "lux.i32");

    SimulationDescriptionBuilder builder;
    assert(builder.addSystem("bridge", kBridgeSystem));
    auto description = std::move(builder).build();
    assert(description);

    Asset asset;
    std::array<std::uint8_t, 16U> id_bytes{};
    id_bytes[0] = 0xA5U;
    asset.id = lux::asset::AssetId{id_bytes};
    asset.content.description = projected->description();

    const auto symbol = projected->description().exports[0].symbol_id;
    const ScriptMountDescription mount{
        ScriptMountId{1U},
        asset.id,
        {{symbol, SystemHookBindingTarget{systemTypeId(kBridgeSystem.canonical_name), "bridge", "value"}}}};
    ecs::Registry registry;
    const auto entity = registry.create();
    registry.emplace<ScriptComponent>(entity, ScriptComponent{{mount}});

    const std::array descriptor_set{std::addressof(*projected)};
    CppStaticScriptBindingBackend backend{descriptor_set, 1U};
    assert(backend);
    const auto backend_descriptor = backend.descriptor();
    ScriptInstanceHostContext contract_host;
    const ScriptInstanceCreateContext contract_context{asset.id, mount.id, entity, &contract_host};
    const auto expect_contract_mismatch = [&](const auto& content) {
        ScriptBackendInstance instance;
        assert(
            backend_descriptor.createInstance(backend_descriptor.context, contract_context, content, instance) ==
            EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH);
        assert(!instance);
    };
    {
        auto tampered = asset.content;
        tampered.description.module_name = "lux.test.tampered";
        expect_contract_mismatch(tampered);
    }
    {
        auto tampered = asset.content;
        tampered.description.model = lux::rdesc::EScriptModel::GLOBAL_MODULE;
        expect_contract_mismatch(tampered);
    }
    {
        auto tampered = asset.content;
        std::get<lux::rdesc::CppStaticScript>(tampered.description.body).descriptor = "wrong-key";
        expect_contract_mismatch(tampered);
    }
    {
        auto tampered = asset.content;
        tampered.description.exports[0].name = "renamed";
        expect_contract_mismatch(tampered);
    }
    {
        auto tampered = asset.content;
        ++tampered.description.exports[0].symbol_id;
        expect_contract_mismatch(tampered);
    }
    {
        auto tampered = asset.content;
        tampered.description.exports[0].args[0].pass = lux::script::EScriptPassMode::CONST_REF;
        expect_contract_mismatch(tampered);
    }
    auto session_result = ScriptBindingSession::create(
        std::move(*description),
        registry,
        ScriptBindingCapacities{1U, 2U, 64U, 4U, 4U, 4U, 4U},
        ScriptAssetResolver{&asset, &resolveAsset},
        std::span{&backend_descriptor, 1U}
    );
    assert(session_result);
    auto session = std::move(*session_result);
    assert(session.prepare());

    float value{3.5F};
    lux_script_value_slot
        slot{LUX_SCRIPT_VK_FLOAT, {}, sizeof(value), lux::script::scriptSemanticTypeId("lux.f32"), &value};
    lux_script_call_frame frame{&slot, 1U, 0U, nullptr, 0U, 0U, nullptr, nullptr};
    const auto hook = session.hookSlot("bridge", "value");
    assert(hook);
    const auto dispatched = session.dispatchHook(hook, frame);
    assert(dispatched.calls == 1U);
    assert(test::observed_self == entity);
    assert(test::observed_value == value);
    const auto cold_asset_resolutions = session.instrumentation().asset_resolutions;
    const auto cold_target_resolutions = session.instrumentation().target_resolutions;
    assert(session.dispatchHook(hook, frame).calls == 1U);
    assert(session.instrumentation().asset_resolutions == cold_asset_resolutions);
    assert(session.instrumentation().target_resolutions == cold_target_resolutions);
    assert(session.shutdown());
    lux::meta::ReflectionRegistry::destroyRegistry();
}

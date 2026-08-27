#include <lux/engine/authoring/ScriptBindingAuthoring.hpp>
#include <lux/engine/authoring/ScriptAuthoringSemanticCatalog.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationStepInfo.hpp>

#include <array>
#include <cassert>
#include <fstream>
#include <iterator>

namespace
{
    using namespace lux::simulation;

    inline constexpr std::array kHooks{
        makeSystemHookPoint<void(const SimulationStepInfo&)>("before"),
        makeSystemHookPoint<void(const SimulationStepInfo&)>("after"),
        makeSystemHookPoint<void()>(
            "single",
            ESystemHookCardinality::SINGLE
        )};
    inline constexpr SystemDescription kSystem{
        .canonical_name = "lux.test.authoring",
        .version = 1U,
        .hooks = kHooks};
}

int main()
{
    using namespace lux::simulation;
    using namespace lux::authoring;

    SimulationDescriptionBuilder builder;
    assert(builder.addSystem("fixture", kSystem));
    auto simulation = std::move(builder).build();
    assert(simulation);

    lux::rdesc::Script script;
    script.module_name = "authoring.fixture";
    script.model = lux::rdesc::EScriptModel::ENTITY_BEHAVIOR;
    script.body = lux::rdesc::CppStaticScript{"fixture"};
    script.exports = {{
        "update",
        1U,
        {{
            "lux.simulation.SimulationStepInfo",
            lux::script::scriptSemanticTypeId(
                "lux.simulation.SimulationStepInfo"
            ),
            lux::script::EScriptPassMode::CONST_REF
        }},
        {}}};
    assert(lux::rdesc::validScriptDescription(script));

    const auto catalog = makeScriptBindingTargetCatalog(*simulation);
    const auto compatible = compatibleScriptBindingTargets(
        *simulation,
        script,
        1U,
        catalog
    );
    assert(compatible.size() == 2U);

    std::array<std::uint8_t, 16U> asset_bytes{};
    asset_bytes[0] = 1U;
    ScriptMountDescription mount{
        ScriptMountId{1U},
        lux::asset::AssetId{asset_bytes},
        {}};
    const auto before = ScriptBindingDescription{
        1U,
        SystemHookBindingTarget{
            systemTypeId(kSystem.canonical_name),
            "fixture",
            "before"}};
    const auto after = ScriptBindingDescription{
        1U,
        SystemHookBindingTarget{
            systemTypeId(kSystem.canonical_name),
            "fixture",
            "after"}};
    assert(addScriptBinding(*simulation, script, mount, before) ==
        EScriptBindingAuthoringError::SUCCESS);
    assert(addScriptBinding(*simulation, script, mount, after) ==
        EScriptBindingAuthoringError::SUCCESS);
    assert(mount.bindings.size() == 2U);
    assert(addScriptBinding(*simulation, script, mount, before) ==
        EScriptBindingAuthoringError::DUPLICATE_BINDING);
    assert(moveScriptBinding(mount, 1U, 0U) ==
        EScriptBindingAuthoringError::SUCCESS);
    assert(mount.bindings[0] == after);
    assert(eraseScriptBinding(mount, 1U) ==
        EScriptBindingAuthoringError::SUCCESS);

    mount.bindings.push_back(ScriptBindingDescription{
        99U,
        SystemHookBindingTarget{
            systemTypeId(kSystem.canonical_name),
            "fixture",
            "missing"}});
    const auto diagnostics = diagnoseScriptBindings(
        *simulation,
        script,
        mount
    );
    assert(diagnostics.size() == 1U);
    assert(diagnostics[0].error ==
        EScriptBindingAuthoringError::MISSING_SYMBOL);

    lux::rdesc::Script global;
    global.module_name = "authoring.global";
    global.model = lux::rdesc::EScriptModel::GLOBAL_MODULE;
    global.body = lux::rdesc::CppStaticScript{"global"};
    global.exports = {
        {"first", 2U, {}, {}},
        {"second", 3U, {}, {}}};
    const auto single_target = SystemHookBindingTarget{
        systemTypeId(kSystem.canonical_name),
        "fixture",
        "single"};
    ScriptMountDescription first_mount{
        ScriptMountId{2U},
        lux::asset::AssetId{asset_bytes},
        {{2U, single_target}}};
    ScriptMountDescription second_mount{
        ScriptMountId{3U},
        lux::asset::AssetId{asset_bytes},
        {{3U, single_target}}};
    const std::array composition{
        ScriptBindingCompositionEntry{&global, &first_mount},
        ScriptBindingCompositionEntry{&global, &second_mount}};
    const auto composition_diagnostics =
        diagnoseScriptBindingComposition(*simulation, composition);
    assert(composition_diagnostics.size() == 1U);
    assert(composition_diagnostics[0].error ==
        EScriptBindingAuthoringError::SINGLE_HOOK_MULTIPLE_HANDLERS);
    assert(composition_diagnostics[0].mount_index == 1U);
    assert(addScriptBinding(
        *simulation,
        global,
        first_mount,
        ScriptBindingDescription{3U, single_target}
    ) == EScriptBindingAuthoringError::SINGLE_HOOK_MULTIPLE_HANDLERS);

    auto semantic_catalog = makeBaseScriptAuthoringSemanticCatalog();
    assert(semantic_catalog);
    assert(semantic_catalog->find("lux.f32"));
    assert(semantic_catalog->find(
        "lux.simulation.SimulationStepInfo"));
    std::ifstream catalog_file(
        LUX_SCRIPT_SEMANTIC_CATALOG,
        std::ios::binary
    );
    assert(catalog_file);
    const std::string installed_catalog{
        std::istreambuf_iterator<char>{catalog_file},
        std::istreambuf_iterator<char>{}};
    assert(semantic_catalog->canonicalJson() + "\n" == installed_catalog);
}

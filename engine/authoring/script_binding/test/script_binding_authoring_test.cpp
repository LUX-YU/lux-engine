#include <lux/engine/authoring/ScriptBindingAuthoring.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationStepInfo.hpp>

#include <array>
#include <cassert>

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
}

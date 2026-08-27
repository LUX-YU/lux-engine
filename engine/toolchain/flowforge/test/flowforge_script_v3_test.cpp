#include <lux/engine/toolchain/flowforge/ScriptCompiler.hpp>

#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationStepInfo.hpp>

#include <array>
#include <cassert>

namespace
{
    inline constexpr std::array kHooks{
        lux::simulation::makeSystemHookPoint<void(
            const lux::simulation::SimulationStepInfo&)>("after")};
    inline constexpr lux::simulation::SystemDescription kSystem{
        .canonical_name = "lux.flowforge.fixture",
        .version = 1U,
        .hooks = kHooks};
}

int main()
{
    using namespace lux::flowforge;
    using namespace lux::simulation;

    SimulationDescriptionBuilder builder;
    assert(builder.addSystem("physics", kSystem));
    assert(builder.addSystem("animation", kSystem));
    auto simulation = std::move(builder).build();
    assert(simulation);

    const auto step = lux::script::makeScriptSemanticType<
        SimulationStepInfo>(lux::script::EScriptPassMode::CONST_REF);
    const std::array exports{
        ExportMethodNode{FlowForgeExportNodeId{1U}, "tick", {step}, {}},
        ExportMethodNode{FlowForgeExportNodeId{2U}, "idle", {}, {}},
        ExportMethodNode{FlowForgeExportNodeId{3U}, "once", {step}, {}}};
    const auto type = systemTypeId(kSystem.canonical_name);
    const std::array bindings{
        BindingEdge{
            FlowForgeExportNodeId{1U},
            SystemHookBindingTarget{type, "physics", "after"}},
        BindingEdge{
            FlowForgeExportNodeId{1U},
            SystemHookBindingTarget{type, "animation", "after"}},
        BindingEdge{
            FlowForgeExportNodeId{3U},
            SystemHookBindingTarget{type, "physics", "after"}}};

    auto compiled = compileFlowForgeScript(
        "gameplay.behavior",
        lux::rdesc::EScriptModel::ENTITY_BEHAVIOR,
        exports,
        bindings,
        *simulation,
        FlowForgeStateLayout{
            0x1234U,
            16U,
            16U,
            {std::byte{1U}}}
    );
    assert(compiled);
    assert(compiled->description.exports.size() == 3U);
    assert(compiled->binding_template.size() == 3U);
    assert(compiled->binding_template[0].function ==
        compiled->binding_template[1].function);
    assert(compiled->binding_template[0].function !=
        compiled->binding_template[2].function);
    assert(compiled->description.exports[1].name == "idle");
    assert(compiled->abi.abi_version == LUX_SCRIPT_ABI_VERSION);
    assert(compiled->abi.state.align == 16U);
    assert(lux::rdesc::validScriptDescription(compiled->description));
}

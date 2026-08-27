#include <lux/engine/toolchain/flowforge/ScriptCompiler.hpp>

#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationStepInfo.hpp>

#include <array>
#include <cassert>

namespace
{
    inline constexpr std::array kHooks{
        lux::simulation::makeSystemHookPoint<void(const lux::simulation::SimulationStepInfo&)>("after"),
        lux::simulation::makeSystemHookPoint<void(float)>("value-f32")};
    inline constexpr lux::simulation::SystemDescription kSystem{
        .canonical_name = "lux.flowforge.fixture",
        .version = 1U,
        .hooks = kHooks};
}

int
main()
{
    using namespace lux::flowforge;
    using namespace lux::simulation;

    SimulationDescriptionBuilder builder;
    assert(builder.addSystem("physics", kSystem));
    assert(builder.addSystem("animation", kSystem));
    auto simulation = std::move(builder).build();
    assert(simulation);

    const lux::rdesc::ScriptValueType step{
        "lux.simulation.SimulationStepInfo",
        lux::script::scriptSemanticTypeId("lux.simulation.SimulationStepInfo"),
        lux::script::EScriptPassMode::CONST_REF};
    const lux::rdesc::ScriptValueType i32{
        "lux.i32",
        lux::script::scriptSemanticTypeId("lux.i32"),
        lux::script::EScriptPassMode::VALUE};
    const lux::rdesc::ScriptValueType f32{
        "lux.f32",
        lux::script::scriptSemanticTypeId("lux.f32"),
        lux::script::EScriptPassMode::VALUE};
    const std::array exports{
        ExportMethodNode{FlowForgeExportNodeId{1U}, "tick", {step}, {}},
        ExportMethodNode{FlowForgeExportNodeId{2U}, "idle", {}, {}},
        ExportMethodNode{FlowForgeExportNodeId{3U}, "once", {step}, {}},
        ExportMethodNode{FlowForgeExportNodeId{4U}, "foo", {i32}, {}},
        ExportMethodNode{FlowForgeExportNodeId{5U}, "foo", {f32}, {}}};
    const auto type = systemTypeId(kSystem.canonical_name);
    const std::array bindings{
        BindingEdge{FlowForgeExportNodeId{1U}, SystemHookBindingTarget{type, "physics", "after"}},
        BindingEdge{FlowForgeExportNodeId{1U}, SystemHookBindingTarget{type, "animation", "after"}},
        BindingEdge{FlowForgeExportNodeId{3U}, SystemHookBindingTarget{type, "physics", "after"}},
        BindingEdge{FlowForgeExportNodeId{5U}, SystemHookBindingTarget{type, "physics", "value-f32"}}};

    auto compiled = compileFlowForgeScript(
        "gameplay.behavior",
        lux::rdesc::EScriptModel::ENTITY_BEHAVIOR,
        exports,
        bindings,
        *simulation,
        FlowForgeStateLayout{0x1234U, 16U, 16U, {std::byte{1U}}}
    );
    assert(compiled);
    assert(compiled->description.exports.size() == 5U);
    assert(compiled->binding_template.size() == 4U);
    assert(compiled->binding_template[0].function == compiled->binding_template[1].function);
    assert(compiled->binding_template[0].function != compiled->binding_template[2].function);
    assert(compiled->binding_template[3].function == compiled->description.exports[4].symbol_id);
    assert(compiled->binding_template[3].function != compiled->description.exports[3].symbol_id);
    assert(compiled->description.exports[1].name == "idle");
    assert(compiled->abi.abi_version == LUX_SCRIPT_ABI_VERSION);
    assert(compiled->abi.state.align == 16U);
    assert(lux::rdesc::validScriptDescription(compiled->description));
}

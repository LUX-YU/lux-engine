#include <lux/engine/flowforge/compiler/ScriptArtifactCompiler.hpp>
#include <lux/engine/function/script/abi/lux_script_abi.h>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationStepInfo.hpp>

#include <array>
#include <cassert>

namespace
{
    inline constexpr lux::simulation::HookPointId AfterHook{11U};
    inline constexpr lux::simulation::HookPointId ValueHook{12U};
    inline constexpr lux::simulation::SystemInstanceId PhysicsSystem{31U};
    inline constexpr lux::simulation::SystemInstanceId AnimationSystem{32U};
    inline constexpr std::array Hooks{
        lux::simulation::makeHookPointSpec<void(
            const lux::simulation::SimulationStepInfo&)>(
                AfterHook,
                "after"),
        lux::simulation::makeHookPointSpec<void(float)>(
            ValueHook,
            "value-f32")};
    inline constexpr lux::simulation::SystemDescription System{
        .canonical_name = "lux.flowforge.fixture",
        .version = 1U,
        .hooks = Hooks};
}

int main()
{
    using namespace lux::flowforge;
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    SimulationDescriptionBuilder builder;
    assert(builder.addSystem(PhysicsSystem, "physics", System));
    assert(builder.addSystem(AnimationSystem, "animation", System));
    auto simulation = std::move(builder).build();
    assert(simulation);
    const auto physics = simulation->findSystem("physics");
    const auto animation = simulation->findSystem("animation");
    assert(physics && animation);

    const auto step = lux::rdesc::makeScriptValueType<SimulationStepInfo>(
        lux::script::EScriptPassMode::CONST_REF);
    const auto i32 = lux::rdesc::makeScriptValueType<std::int32_t>();
    const auto f32 = lux::rdesc::makeScriptValueType<float>();
    const std::array exports{
        ExportMethodNode{FlowForgeExportNodeId{1U}, 11U, "tick", {step}, {}},
        ExportMethodNode{FlowForgeExportNodeId{2U}, 12U, "idle", {}, {}},
        ExportMethodNode{FlowForgeExportNodeId{3U}, 13U, "once", {step}, {}},
        ExportMethodNode{FlowForgeExportNodeId{4U}, 14U, "foo", {i32}, {}},
        ExportMethodNode{FlowForgeExportNodeId{5U}, 15U, "foo", {f32}, {}}};
    const std::array bindings{
        BindingEdge{
            FlowForgeExportNodeId{1U},
            HookScriptTarget{physics.instanceId(), AfterHook}},
        BindingEdge{
            FlowForgeExportNodeId{1U},
            HookScriptTarget{animation.instanceId(), AfterHook}},
        BindingEdge{
            FlowForgeExportNodeId{3U},
            HookScriptTarget{physics.instanceId(), AfterHook}},
        BindingEdge{
            FlowForgeExportNodeId{5U},
            HookScriptTarget{physics.instanceId(), ValueHook}}};

    auto compiled = compileFlowForgeScript(
        "gameplay.behavior",
        false,
        exports,
        bindings,
        *simulation,
        FlowForgeStateLayout{0x1234U, 16U, 16U, {std::byte{1U}}});
    assert(compiled);
    assert(compiled->description.exports.size() == 5U);
    assert(compiled->binding_template.size() == 4U);
    assert(compiled->binding_template[0].symbol ==
        compiled->binding_template[1].symbol);
    assert(compiled->binding_template[0].symbol !=
        compiled->binding_template[2].symbol);
    assert(compiled->binding_template[3].symbol ==
        compiled->description.exports[4].symbol_id);
    assert(compiled->binding_template[3].symbol !=
        compiled->description.exports[3].symbol_id);
    assert(compiled->description.exports[1].name == "idle");
    assert(compiled->abi.abi_version == LUX_SCRIPT_ABI_VERSION);
    assert(compiled->abi.state.align == 16U);
    assert(lux::rdesc::validScriptDescription(compiled->description));
    return 0;
}

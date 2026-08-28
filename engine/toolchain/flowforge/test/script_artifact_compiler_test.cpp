#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/function/script/abi/lux_script_abi.h>

#include <array>
#include <cassert>

int main()
{
    using namespace lux::flowforge;

    const auto i32 = lux::rdesc::makeScriptValueType<std::int32_t>();
    const auto f32 = lux::rdesc::makeScriptValueType<float>();
    const std::array exports{
        ExportMethodNode{FlowForgeExportNodeId{1U}, 11U, "tick", {i32}, {}},
        ExportMethodNode{FlowForgeExportNodeId{2U}, 12U, "idle", {}, {}},
        ExportMethodNode{FlowForgeExportNodeId{3U}, 13U, "once", {f32}, {}},
        ExportMethodNode{FlowForgeExportNodeId{4U}, 14U, "foo", {i32}, {}},
        ExportMethodNode{FlowForgeExportNodeId{5U}, 15U, "foo", {f32}, {}}
    };

    auto compiled = compileFlowForgeScript(
        "gameplay.behavior",
        exports,
        StateLayout{{}, 16U, 16U, 0x1234U, {std::byte{1U}}}
    );
    assert(compiled);
    assert(compiled->description().exports.size() == 5U);
    assert(compiled->description().exports[1].name == "idle");
    const auto& native = std::get<lux::rdesc::NativeModuleScript>(compiled->description().body);
    assert(native.abi_version == LUX_SCRIPT_ABI_VERSION);
    assert(native.state_align == 16U);
    assert(lux::rdesc::validScriptDescription(compiled->description()));
    assert(compiled->findExport(15U) == &compiled->description().exports[4U]);

    auto duplicate_exports = exports;
    duplicate_exports[4].symbol = duplicate_exports[3].symbol;
    auto duplicate = compileFlowForgeScript(
        "gameplay.behavior",
        duplicate_exports,
        StateLayout{{}, 16U, 16U, 0x1234U, {}}
    );
    assert(!duplicate);
    assert(duplicate.error() == EFlowForgeCompileError::INVALID_GRAPH);
    return 0;
}

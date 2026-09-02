#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/function/script/native/NativeModule.hpp>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <memory>
#include <vector>

namespace
{
    lux::flowforge::FlowGraph makeGraph(
        std::string_view display_name,
        lux::script::ScriptSymbolId symbol
    )
    {
        lux::flowforge::FlowGraph graph;
        const auto node_index = graph.addNodes(std::make_unique<lux::flowforge::OnEventNode>(display_name));
        const auto node_id = graph.getNode(node_index).node->id();
        const bool added = graph.addExport(lux::flowforge::ExportMethodNode{
            lux::flowforge::FlowForgeExportNodeId{1U},
            node_id,
            symbol
        });
        assert(added);
        return graph;
    }
}

int main()
{
    constexpr lux::script::ScriptSymbolId Symbol = 0x1234U;
    auto graph = makeGraph("tick", Symbol);
    auto compiled = lux::flowforge::compileFlowForgeScript(
        graph,
        lux::flowforge::FlowForgeCompileOptions{.module_name = "gameplay.behavior"}
    );
    if (!compiled)
        std::fprintf(stderr, "FlowForge compile failed: %u %s\n", static_cast<unsigned>(compiled.error().code),
                     compiled.error().message.c_str());
    assert(compiled);
    assert(!compiled->payload().empty());
    assert(compiled->findExport(Symbol) == &compiled->description().exports.front());

    auto loaded = lux::script::loadNativeModule(compiled->payload(), "gameplay.behavior");
    assert(loaded);
    assert(loaded->findFunction(Symbol) != nullptr);

    lux_script_call_frame frame{};
    assert(loaded->findFunction(Symbol)->invoke(&frame) == 0);

    auto compiled_again = lux::flowforge::compileFlowForgeScript(
        graph,
        lux::flowforge::FlowForgeCompileOptions{.module_name = "gameplay.behavior"}
    );
    assert(compiled_again);
    assert(compiled_again->description().exports == compiled->description().exports);
    assert(std::ranges::equal(compiled_again->payload(), compiled->payload()));

    auto renamed_graph = makeGraph("renamed_tick", Symbol);
    auto renamed = lux::flowforge::compileFlowForgeScript(
        renamed_graph,
        lux::flowforge::FlowForgeCompileOptions{.module_name = "gameplay.behavior"}
    );
    assert(renamed);
    auto renamed_module = lux::script::loadNativeModule(renamed->payload(), "gameplay.behavior");
    assert(renamed_module);
    assert(renamed_module->findFunction(Symbol) != nullptr);

    auto duplicate = makeGraph("first", 1U);
    const auto duplicate_node = duplicate.addNodes(std::make_unique<lux::flowforge::OnEventNode>("second"));
    const auto duplicate_node_id = duplicate.getNode(duplicate_node).node->id();
    assert(duplicate.addExport(lux::flowforge::ExportMethodNode{
        lux::flowforge::FlowForgeExportNodeId{1U},
        duplicate_node_id,
        2U
    }));
    assert(!lux::flowforge::validFlowForgeExports(duplicate));

    lux::flowforge::FlowGraph dangling;
    assert(dangling.addExport(lux::flowforge::ExportMethodNode{
        lux::flowforge::FlowForgeExportNodeId{1U},
        42U,
        1U
    }));
    assert(!lux::flowforge::validFlowForgeExports(dangling));

    lux::flowforge::FlowGraph wrong_entry;
    auto function = std::make_unique<lux::flowforge::FuncDefNode>(
        "helper",
        std::vector<lux::flowforge::FuncArgInfo>{}
    );
    const auto function_node = wrong_entry.addNodes(std::move(function));
    const auto function_node_id = wrong_entry.getNode(function_node).node->id();
    assert(wrong_entry.addExport(lux::flowforge::ExportMethodNode{
        lux::flowforge::FlowForgeExportNodeId{1U},
        function_node_id,
        1U
    }));
    assert(!lux::flowforge::validFlowForgeExports(wrong_entry));
    return 0;
}

#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>

#include <memory>

int main()
{
    lux::flowforge::FlowGraph graph;
    const auto node_index = graph.addNodes(std::make_unique<lux::flowforge::OnEventNode>("tick"));
    const auto node_id = graph.getNode(node_index).node->id();
    if (!graph.addExport(
        lux::flowforge::ExportMethodNode{
            lux::flowforge::FlowForgeExportNodeId{1U},
            node_id,
            1U,
        }
    ))
    {
        return 1;
    }
    auto artifact = lux::flowforge::compileFlowForgeScript(
        graph,
        lux::flowforge::FlowForgeCompileOptions{.module_name = "lux.consumer.flowforge"}
    );
    return artifact && artifact->findExport(1U) ? 0 : 1;
}

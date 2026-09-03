#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/script/ScriptEventAwaitNode.hpp>

#include <memory>
#include <cstdio>
#include <span>
#include <utility>

int main()
{
    lux::flowforge::FlowGraph graph;
    const lux::script::ScriptEventSourceDescription event_source{
        "Gameplay",
        "damage",
        41U,
        42U,
        lux::script::EScriptEventRoute::SIMULATION_BROADCAST,
        {"lux.i32", lux::semantic::typeId("lux.i32"), LUX_SCRIPT_VK_INT32, 4U, 4U},
        lux::semantic::typeId("lux.i32"),
        1U
    };
    auto event = std::make_unique<lux::flowforge::OnEventNode>("tick");
    auto wait = std::make_unique<lux::flowforge::ScriptEventAwaitNode>(event_source);
    auto* event_pointer = event.get();
    auto* wait_pointer = wait.get();
    const auto node_index = graph.addNodes(std::move(event));
    graph.addNodes(std::move(wait));
    lux::flowforge::LastLink previous;
    if (event_pointer->execOutPin().linkTo(&wait_pointer->execInPin(), previous) !=
        lux::flowforge::ELinkError::SUCCESS)
    {
        return 1;
    }
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
        lux::flowforge::FlowForgeCompileOptions{
            .module_name = "lux.consumer.flowforge",
            .script_events = std::span{&event_source, 1U}
        }
    );
    if (!artifact)
        std::fprintf(stderr, "FlowForge consumer compile failed: %u %s\n",
                     static_cast<unsigned>(artifact.error().code), artifact.error().message.c_str());
    if (!artifact || !artifact->findExport(1U))
        return 1;
    const auto module = lux::script::loadNativeModule(artifact->payload(), "lux.consumer.flowforge");
    return module && module->eventWaitImports().size() == 1U ? 0 : 1;
}

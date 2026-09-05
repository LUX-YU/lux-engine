#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/script/ScriptEventAwaitNode.hpp>
#include <lux/engine/function/script/native/NativeModule.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/scripting/ScriptEventSource.hpp>

#include <memory>
#include <array>
#include <cstdio>
#include <span>
#include <utility>

int main()
{
    lux::flowforge::FlowGraph graph;
    using namespace lux::simulation;
    constexpr lux::system::SystemInstanceId owner{41U};
    constexpr EventPointId damage{42U};
    constexpr HookPointId delivery{43U};
    constexpr std::array hooks{makeHookPointSpec<void()>(delivery, "delivery", true, true)};
    constexpr std::array events{makeEventPointSpec<std::int32_t>(damage, "damage", delivery,
        EEventRoute::SIMULATION_BROADCAST, "lux.i32", 1U)};
    const SimulationSystemDescription descriptor{
        .type = {.canonical_name = "consumer.gameplay", .version = 1U}, .hooks = hooks, .events = events};
    SimulationDescriptionBuilder builder;
    const bool added_system = static_cast<bool>(builder.addSystem(owner, "Gameplay", descriptor));
    const bool added_execution = added_system && static_cast<bool>(builder.addExecutionDependency(
        SimulationExecutionPoint::task(owner), SimulationExecutionPoint::hook(owner, delivery)));
    const bool added_producer = added_execution &&
        static_cast<bool>(builder.addChannelProducer({owner, damage, owner, PrimarySimulationTask}));
    if (!added_producer)
        return 1;
    auto description = std::move(builder).build();
    if (!description)
        return 1;
    auto projected = lux::simulation::script::describeScriptEventSource<std::int32_t>(
        description->findEvent(owner, damage));
    if (!projected)
        return 1;
    const auto event_source = *projected;
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

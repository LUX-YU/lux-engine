#include "PhysicsQuery2D.ability.generated.hpp"
#include "Physics2DScriptTestSupport.hpp"

#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityCatalog.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityNode.hpp>
#include <lux/engine/flowforge/script/ScriptEventAwaitNode.hpp>
#include <lux/engine/function/script/artifact/ScriptArtifact.hpp>
#include <lux/engine/physics2d/abilities/PhysicsQuery2D.hpp>
#include <lux/engine/simulation/abilities/DelayAbility.hpp>

#include "DelayAbility.ability.generated.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>

int main(int argc, char** argv)
{
    using namespace lux;
    using namespace lux::physics2d;
    using namespace lux::physics2d::test;
    if (argc != 2)
        return 1;

    flowforge::ScriptAbilityNodeCatalog catalog;
    if (!catalog.add(flowforge::makeScriptAbilityCatalogContribution<PhysicsQuery2D>()) ||
        !catalog.add(flowforge::makeScriptAbilityCatalogContribution<simulation::script::DelayAbility>()))
    {
        return 2;
    }
    const auto* physics = catalog.view().find(script::ScriptAbilityTraits<PhysicsQuery2D>::Description.id,
                                              script::ScriptAbilityTraits<PhysicsQuery2D>::Methods.front().id);
    const auto* next_step = catalog.view().find(
        script::ScriptApiContractIdView{"lux.simulation.delay"},
        script::ScriptApiMethodIdView{"lux.simulation.delay.next_step"}
    );
    if (physics == nullptr || next_step == nullptr)
        return 3;

    flowforge::FlowGraph graph;
    auto entry = std::make_unique<flowforge::OnEventNode>("tick");
    auto overlap = std::make_unique<flowforge::ScriptAbilityNode>(*physics);
    const auto event_source = pulseEventSource();
    auto event_wait = std::make_unique<flowforge::ScriptEventAwaitNode>(event_source);
    auto delay = std::make_unique<flowforge::ScriptAbilityNode>(*next_step);
    for (auto& parameter : overlap->parameterPins())
    {
        if (!parameter->setConstantData(meta::RuntimeObject(double{0.25})))
            return 4;
    }
    if (!overlap->parameterPins()[0]->setConstantData(meta::RuntimeObject(double{0.0})) ||
        !overlap->parameterPins()[1]->setConstantData(meta::RuntimeObject(double{0.0})))
    {
        return 5;
    }
    auto* entry_pointer = entry.get();
    auto* overlap_pointer = overlap.get();
    auto* event_wait_pointer = event_wait.get();
    auto* delay_pointer = delay.get();
    const auto entry_slot = graph.addNodes(std::move(entry));
    graph.addNodes(std::move(overlap));
    graph.addNodes(std::move(event_wait));
    graph.addNodes(std::move(delay));
    flowforge::LastLink previous;
    if (entry_pointer->execOutPin().linkTo(&overlap_pointer->execInPin(), previous) != flowforge::ELinkError::SUCCESS)
    {
        return 6;
    }
    if (overlap_pointer->execOutPin().linkTo(&event_wait_pointer->execInPin(), previous) !=
        flowforge::ELinkError::SUCCESS)
    {
        return 7;
    }
    if (event_wait_pointer->execOutPin().linkTo(&delay_pointer->execInPin(), previous) !=
        flowforge::ELinkError::SUCCESS)
    {
        return 8;
    }
    if (!graph.addExport({flowforge::FlowForgeExportNodeId{1U}, graph.getNode(entry_slot).node->id(), FlowTickSymbol}))
    {
        return 9;
    }

    auto artifact = flowforge::compileFlowForgeScript(
        graph,
        {
            .module_name = "lux.physics2d.flowforge-benchmark",
            .script_abilities = catalog.view(),
            .script_events = std::span{&event_source, 1U}
        }
    );
    if (!artifact)
        return 10;
    std::array<std::uint8_t, 16U> id_bytes{};
    id_bytes.front() = 0x2DU;
    id_bytes.back() = 0xF0U;
    auto typed = script::ScriptArtifactAsset::create(
        asset::AssetInfo{asset::AssetId{id_bytes}, script::ScriptArtifactAsset::asset_type, 0U},
        std::make_shared<const script::ScriptArtifact>(std::move(*artifact)));
    if (!typed)
        return 11;
    const auto encoded =
        asset::TAssetSerDeser<script::ScriptArtifactAsset>::encode(**typed,
                                                                   asset::AssetEncodeLimits{64U * 1024U * 1024U});
    if (!encoded)
        return 12;
    std::ofstream output(std::filesystem::path{argv[1]}, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(encoded->data()), static_cast<std::streamsize>(encoded->size()));
    return output ? 0 : 13;
}

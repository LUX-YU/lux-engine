#include "PhysicsQuery2D.ability.generated.hpp"
#include "Physics2DScriptTestSupport.hpp"

#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityCatalog.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityNode.hpp>
#include <lux/engine/function/script/artifact/ScriptArtifact.hpp>
#include <lux/engine/physics2d/abilities/PhysicsQuery2D.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>

int main(int argc, char** argv)
{
    using namespace lux;
    using namespace lux::physics2d;
    using namespace lux::physics2d::test;
    if (argc != 2)
        return 1;

    flowforge::ScriptAbilityNodeCatalog catalog;
    if (!catalog.add(flowforge::makeScriptAbilityCatalogContribution<PhysicsQuery2D>()))
        return 2;
    const auto* physics = catalog.view().find(script::ScriptAbilityTraits<PhysicsQuery2D>::Description.id,
                                              script::ScriptAbilityTraits<PhysicsQuery2D>::Methods.front().id);
    if (physics == nullptr)
        return 3;

    flowforge::FlowGraph graph;
    auto entry = std::make_unique<flowforge::OnEventNode>("tick");
    auto overlap = std::make_unique<flowforge::ScriptAbilityNode>(*physics);
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
    const auto entry_slot = graph.addNodes(std::move(entry));
    graph.addNodes(std::move(overlap));
    flowforge::LastLink previous;
    if (entry_pointer->execOutPin().linkTo(&overlap_pointer->execInPin(), previous) != flowforge::ELinkError::SUCCESS)
    {
        return 6;
    }
    if (!graph.addExport({flowforge::FlowForgeExportNodeId{1U}, graph.getNode(entry_slot).node->id(), FlowTickSymbol}))
    {
        return 7;
    }

    auto artifact = flowforge::compileFlowForgeScript(
        graph,
        {.module_name = "lux.physics2d.flowforge-benchmark", .script_abilities = catalog.view()});
    if (!artifact)
        return 8;
    std::array<std::uint8_t, 16U> id_bytes{};
    id_bytes.front() = 0x2DU;
    id_bytes.back() = 0xF0U;
    auto typed = script::ScriptArtifactAsset::create(
        asset::AssetInfo{asset::AssetId{id_bytes}, script::ScriptArtifactAsset::asset_type, 0U},
        std::make_shared<const script::ScriptArtifact>(std::move(*artifact)));
    if (!typed)
        return 9;
    const auto encoded =
        asset::TAssetSerDeser<script::ScriptArtifactAsset>::encode(**typed,
                                                                   asset::AssetEncodeLimits{64U * 1024U * 1024U});
    if (!encoded)
        return 10;
    std::ofstream output(std::filesystem::path{argv[1]}, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(encoded->data()), static_cast<std::streamsize>(encoded->size()));
    return output ? 0 : 11;
}

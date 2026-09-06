#include "CounterAbility.ability.generated.hpp"
#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/graph/ArithmeticNode.hpp>
#include <lux/engine/flowforge/graph/ObjectNode.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityNode.hpp>
#include <nlohmann/json.hpp>
#include <array>
#include <fstream>

int main(int argc, char** argv)
{
    if (argc != 3) return 1;
    using namespace lux;
    flowforge::ScriptAbilityNodeCatalog catalog;
    if (!catalog.add(flowforge::makeScriptAbilityCatalogContribution<authoring_consumer::CounterAbility>())) return 2;
    const auto* method = catalog.view().find(script::ScriptApiContractIdView{"consumer.authoring.counter"},
        script::ScriptApiMethodIdView{"consumer.authoring.counter.record"});
    if (!method) return 3;
    const auto* i32 = &meta::ref_type_of_v<std::int32_t>;
    flowforge::FlowGraph graph;
    auto entry = std::make_unique<flowforge::OnEventNode>(
        "observe", std::vector<flowforge::FuncArgInfo>{{i32, "value"}}
    );
    const auto variable = graph.addVariable("sum", i32, meta::RuntimeObject(std::int32_t{}));
    const flowforge::DataPinInfo info{"sum", i32};
    auto get = std::make_unique<flowforge::GetVariableNode>(variable, info);
    auto add = std::make_unique<flowforge::BinaryOpNode>(flowforge::ENodeOperation::ADD, i32);
    auto set = std::make_unique<flowforge::SetVariableNode>(variable, info);
    auto record = std::make_unique<flowforge::ScriptAbilityNode>(*method);
    auto& entry_node = *entry;
    auto& get_node = *get;
    auto& add_node = *add;
    auto& set_node = *set;
    auto& record_node = *record;
    const auto entry_slot = graph.addNodes(std::move(entry));
    graph.addNodes(std::move(get)); graph.addNodes(std::move(add));
    graph.addNodes(std::move(set)); graph.addNodes(std::move(record));
    flowforge::LastLink old;
    const auto link = [&](auto& output, auto& input) {
        return output.linkTo(&input, old) == flowforge::ELinkError::SUCCESS;
    };
    auto& rhs = const_cast<flowforge::DataInPin&>(add_node.rhs());
    auto& lhs = const_cast<flowforge::DataInPin&>(add_node.lhs());
    auto& value_in = const_cast<flowforge::DataInPin&>(set_node.valueIn());
    const bool execution_linked = link(entry_node.execOutPin(), set_node.execInPin()) &&
        link(set_node.execOutPin(), record_node.execInPin());
    const bool values_linked = execution_linked && link(*entry_node.paramPins()[0], rhs) &&
        link(const_cast<flowforge::DataOutPin&>(get_node.valuePin()), lhs) &&
        link(const_cast<flowforge::DataOutPin&>(add_node.result()), value_in) &&
        link(const_cast<flowforge::DataOutPin&>(set_node.valueOut()), *record_node.parameterPins()[0]);
    if (!values_linked) return 4;
    if (!graph.addExport({{1U}, graph.getNode(entry_slot).node->id(), 3U,
        {{script::EScriptBindingHintKind::HOOK, "Host.first"}}})) return 5;
    auto artifact = flowforge::compileFlowForgeScript(graph,
        {.module_name = "consumer.authoring.flow", .script_abilities = catalog.view()});
    auto hints = flowforge::describeFlowForgeBindingHints(graph);
    if (!artifact || !hints) return 6;
    std::array<std::uint8_t, 16U> bytes{};
    bytes[0] = 0x33U;
    auto typed = script::ScriptArtifactAsset::create(
        {asset::AssetId{bytes}, script::ScriptArtifactAsset::asset_type, 0U},
        std::make_shared<const script::ScriptArtifact>(std::move(*artifact))
    );
    if (!typed) return 7;
    auto encoded = asset::TAssetSerDeser<script::ScriptArtifactAsset>::encode(**typed,
        asset::AssetEncodeLimits{16U * 1024U * 1024U});
    if (!encoded) return 8;
    std::ofstream output(argv[1], std::ios::binary);
    output.write(reinterpret_cast<const char*>(encoded->data()), static_cast<std::streamsize>(encoded->size()));
    nlohmann::json document{{"schema", "lux-script-binding-suggestions"}, {"version", 1},
        {"module", "consumer.authoring.flow"}, {"suggestions", nlohmann::json::array()}};
    for (const auto& hint : *hints)
        document["suggestions"].push_back({
            {"symbol", hint.symbol}, {"kind", "hook"}, {"target", hint.target.qualified_name}
        });
    std::ofstream metadata(argv[2]);
    metadata << document.dump(2) << '\n';
    return output && metadata ? 0 : 9;
}

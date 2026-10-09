#include "FlowGraphFixture.hpp"
#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/flowforge/FunctionNodes.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityPayload.hpp>

#include <algorithm>
#include <cstdio>
#include <string>

int main()
{
    using namespace lux::flowforge;
    using test::require;
    test::GraphFixture fixture;
    FlowGraph graph;
    const auto entry = fixture.add(graph, "lux.flow.event", EventEntryPayload{}, "Tick");
    require(graph.addExport({{1}, entry, 41, {}}));
    FlowForgeCompileOptions options{.module_name = "ability_contract"};
    auto empty = compileFlowForgeObject(graph, options);
    require(empty && !empty->object.empty());

    FlowNodeCatalog node_catalog;
    const std::array registrations{scriptAbilityRegistration()};
    require(node_catalog.add(registrations).has_value());
    auto ability_type = node_catalog.find(lux::graph::nodeTypeId("lux.flow.ability_call"));
    // A claimed intrinsic type cannot authorize a concrete payload cast.
    require(!createFlowNode(ability_type, {}));
    const auto impostor = fixture.add(graph, "lux.flow.start", StartPayload{});
    auto* malformed = const_cast<FlowNode*>(graph.node(impostor));
    malformed->definition = ability_type;
    malformed->payload = {};
    auto invalid = compileFlowForgeObject(graph, options);
    require(!invalid && invalid.error().code == EFlowForgeError::GRAPH_INVALID);
    require(graph.removeNode(impostor).has_value());

    ScriptAbilityNodeDescription description{
        .contract = lux::script::ScriptApiContractIdView{"test.runtime"},
        .method = lux::script::ScriptApiMethodIdView{"ping"},
        .contract_display_name = "Test",
        .method_display_name = "Ping",
        .schema_hash = 77
    };
    const auto add_ability = [&]()
    {
        auto payload = FlowNodePayload::make<ScriptAbilityPayload, test::clone<ScriptAbilityPayload>>(
            lux::object::CodeLease::builtin(),
            description
        );
        require(payload.has_value());
        auto node = createFlowNode(ability_type, std::move(*payload));
        require(node.has_value());
        require(node->payload.get<ScriptAbilityPayload>() != nullptr);
        auto added = graph.addNode(std::move(*node));
        require(added.has_value());
        return *added;
    };
    const auto ability = add_ability();
    const auto entry_pin = test::execOut(graph, entry);
    const auto ability_pin = test::execIn(graph, ability);
    require(graph.connect(entry_pin, ability_pin).has_value());

    // A rejected replacement preserves topology; explicit edits can replace and restore it.
    const auto alternative = add_ability();
    const auto alternative_pin = test::execIn(graph, alternative);
    const auto refused = graph.connect(entry_pin, alternative_pin);
    require(!refused);
    require(
        std::get<lux::graph::GraphTopologyFailure>(refused.error()).code ==
        lux::graph::EGraphTopologyError::FAN_CAP_EXCEEDED
    );
    require(graph.topology().findLink(entry_pin, ability_pin));
    require(graph.disconnect(entry_pin, ability_pin).has_value());
    require(graph.topology().linkCount(entry_pin) == 0);
    require(graph.connect(entry_pin, alternative_pin).has_value());
    require(graph.topology().findLink(entry_pin, alternative_pin));
    require(graph.disconnect(entry_pin, alternative_pin).has_value());
    require(graph.connect(entry_pin, ability_pin).has_value());
    require(graph.removeNode(alternative).has_value());

    auto missing = compileFlowForgeObject(graph, options);
    require(!missing && missing.error().code == EFlowForgeError::UNKNOWN_SCRIPT_ABILITY_CONTRACT);
    require(missing.error().node_id == ability.value);

    auto changed = description;
    changed.schema_hash = 78;
    options.script_abilities = ScriptAbilityNodeCatalogView{{&changed, 1}};
    auto mismatch = compileFlowForgeObject(graph, options);
    require(!mismatch && mismatch.error().code == EFlowForgeError::SCRIPT_ABILITY_SCHEMA_MISMATCH);
    options.script_abilities = ScriptAbilityNodeCatalogView{{&description, 1}};
    auto compiled = compileFlowForgeObject(graph, options);
    require(compiled && !compiled->object.empty());
    require(compiled->description.api_requirements.size() == 1);
    require(compiled->description.api_requirements.front().expected_schema_hash == 77);

    // Actual compiler consumes catalog metadata after the caller's dynamic backing has changed and died.
    ScriptAbilityNodeCatalog catalog;
    {
        std::string contract = "test.runtime";
        std::string method = "ping";
        auto dynamic = description;
        dynamic.contract = lux::script::ScriptApiContractIdView{contract};
        dynamic.method = lux::script::ScriptApiMethodIdView{method};
        require(catalog.add({{&dynamic, 1}}).has_value());
        std::fill(contract.begin(), contract.end(), '#');
        std::fill(method.begin(), method.end(), '#');
    }
    options.script_abilities = catalog.view();
    auto owned = compileFlowForgeObject(graph, options);
    require(owned && !owned->object.empty());
    require(owned->description.api_requirements.size() == 1);
    require(owned->description.api_requirements.front().expected_schema_hash == 77);
    std::puts("PASS real Flow compiler: empty export, impostor rejection, catalog/schema errors and Ability object");
}

#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityNode.hpp>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>

int main()
{
    using namespace lux::flowforge;
    FlowGraph graph;
    auto entry = std::make_unique<OnEventNode>("Tick");
    auto* entry_ptr = entry.get();
    graph.addNode(std::move(entry));
    assert(graph.addExport({{1}, graph.nodeId(entry_ptr), 41, {}}));
    FlowForgeCompileOptions options{.module_name = "ability_contract"};
    auto empty = compileFlowForgeObject(graph, options);
    assert(empty && !empty->object.empty());

    // A caller-supplied operation enum cannot authorize a concrete downcast.
    auto impostor = std::make_unique<Node>(ENodeOperation::SCRIPT_ABILITY_CALL);
    assert(!impostor->scriptAbility());
    auto impostor_index = graph.addNode(std::move(impostor));
    auto invalid = compileFlowForgeObject(graph, options);
    assert(!invalid && invalid.error().code == EFlowForgeError::GRAPH_INVALID);
    assert(graph.removeNode(impostor_index));

    ScriptAbilityNodeDescription description{
        .contract = lux::script::ScriptApiContractIdView{"test.runtime"},
        .method = lux::script::ScriptApiMethodIdView{"ping"},
        .contract_display_name = "Test",
        .method_display_name = "Ping",
        .schema_hash = 77
    };
    auto ability = std::make_unique<ScriptAbilityNode>(description);
    auto* ability_ptr = ability.get();
    assert(ability_ptr->scriptAbility() == ability_ptr);
    graph.addNode(std::move(ability));
    assert(entry_ptr->execOutPin().linkTo(&ability_ptr->execInPin()) == ELinkError::SUCCESS);

    // A rejected replacement preserves topology; explicit edits can replace and restore it.
    auto alternative = std::make_unique<ScriptAbilityNode>(description);
    auto* alternative_ptr = alternative.get();
    const auto alternative_index = graph.addNode(std::move(alternative));
    assert(entry_ptr->execOutPin().linkTo(&alternative_ptr->execInPin()) == ELinkError::HAS_LINKED);
    assert(entry_ptr->execOutPin().nextPin() == &ability_ptr->execInPin());
    assert(entry_ptr->execOutPin().unlinkFrom(&ability_ptr->execInPin()) == ELinkError::UNLINKED);
    assert(entry_ptr->execOutPin().nextPin() == nullptr);
    assert(entry_ptr->execOutPin().linkTo(&alternative_ptr->execInPin()) == ELinkError::SUCCESS);
    assert(entry_ptr->execOutPin().nextPin() == &alternative_ptr->execInPin());
    assert(entry_ptr->execOutPin().unlinkFrom(&alternative_ptr->execInPin()) == ELinkError::UNLINKED);
    assert(entry_ptr->execOutPin().linkTo(&ability_ptr->execInPin()) == ELinkError::SUCCESS);
    assert(graph.removeNode(alternative_index));

    auto missing = compileFlowForgeObject(graph, options);
    assert(!missing && missing.error().code == EFlowForgeError::UNKNOWN_SCRIPT_ABILITY_CONTRACT);
    assert(missing.error().node_id == graph.nodeId(ability_ptr).value);

    auto changed = description;
    changed.schema_hash = 78;
    options.script_abilities = ScriptAbilityNodeCatalogView{{&changed, 1}};
    auto mismatch = compileFlowForgeObject(graph, options);
    assert(!mismatch && mismatch.error().code == EFlowForgeError::SCRIPT_ABILITY_SCHEMA_MISMATCH);
    options.script_abilities = ScriptAbilityNodeCatalogView{{&description, 1}};
    auto compiled = compileFlowForgeObject(graph, options);
    assert(compiled && !compiled->object.empty());
    assert(compiled->description.api_requirements.size() == 1);
    assert(compiled->description.api_requirements.front().expected_schema_hash == 77);

    // Actual compiler consumes catalog metadata after the caller's dynamic backing has changed and died.
    ScriptAbilityNodeCatalog catalog;
    {
        std::string contract = "test.runtime";
        std::string method = "ping";
        auto dynamic = description;
        dynamic.contract = lux::script::ScriptApiContractIdView{contract};
        dynamic.method = lux::script::ScriptApiMethodIdView{method};
        assert(catalog.add({{&dynamic, 1}}));
        std::fill(contract.begin(), contract.end(), '#');
        std::fill(method.begin(), method.end(), '#');
    }
    options.script_abilities = catalog.view();
    auto owned = compileFlowForgeObject(graph, options);
    assert(owned && !owned->object.empty());
    assert(owned->description.api_requirements.size() == 1);
    assert(owned->description.api_requirements.front().expected_schema_hash == 77);
    std::puts("PASS real Flow compiler: empty export, impostor rejection, catalog/schema errors and Ability object");
}

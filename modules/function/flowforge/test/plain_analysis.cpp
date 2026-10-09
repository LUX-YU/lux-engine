#include <lux/engine/flowforge/FlowAnalysis.hpp>
#include <lux/engine/flowforge/FunctionNodes.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityPayload.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace
{
    using namespace lux;
    using namespace lux::flowforge;

    void require(bool value, std::source_location at = std::source_location::current()) noexcept
    {
        if (!value)
        {
            std::fprintf(stderr, "plain analysis contract failed at %u\n", at.line());
            std::exit(42);
        }
    }

    template <class T> FlowForgeResult<std::unique_ptr<T>> clone(const T& value) noexcept
    {
        if constexpr (std::is_same_v<T, ScriptAbilityPayload>)
        {
            return std::make_unique<T>(value.description());
        }
        else
        {
            return std::make_unique<T>(value);
        }
    }

    template <class T, class... Args>
    FlowNode node(const FlowNodeCatalog& catalog, std::string_view name, Args&&... args) noexcept
    {
        auto payload = FlowNodePayload::make<T, clone<T>>(object::CodeLease::builtin(), std::forward<Args>(args)...);
        require(payload.has_value());
        auto result = createFlowNode(catalog.find(graph::nodeTypeId(name)), std::move(*payload));
        require(result.has_value());
        return std::move(*result);
    }

    PinId execution(const FlowGraph& graph, NodeId owner, graph::EPinDirection direction) noexcept
    {
        for (const auto& pin : graph.topology().pins())
        {
            const bool matches = pin.owner == owner && pin.direction == direction;
            if (matches && graph.pin(pin.id)->role == EFlowPinRole::EXECUTION)
            {
                return pin.id;
            }
        }
        return {};
    }
} // namespace

int main()
{
    FlowNodeCatalog catalog;
    require(catalog.add(functionNodeRegistrations()).has_value());
    const auto ability_registration = scriptAbilityRegistration();
    require(catalog.add({&ability_registration, 1}).has_value());
    ScriptAbilityNodeDescription ability{
        script::ScriptApiContractIdView{"test.ability"},
        script::ScriptApiMethodIdView{"wait"},
        "Test ability",
        "Wait",
        1,
        17,
        script::EScriptAbilityReceiverKind::NONE,
        script::EScriptApiMethodKind::ASYNC_OPERATION,
        {},
        {}
    };
    ScriptAbilityNodeCatalog abilities;
    require(abilities.add({{&ability, 1}}).has_value());
    auto function = node<FunctionPayload>(catalog, "lux.flow.function");
    auto callee = node<FunctionPayload>(catalog, "lux.flow.function");
    auto call = node<FunctionCallPayload>(catalog, "lux.flow.function_call", NodeId{2});
    auto wait = node<ScriptAbilityPayload>(catalog, "lux.flow.ability_call", ability);
    FlowGraph graph;
    // The caller appears before its definition; the real callback sees the whole candidate.
    const std::array entries{
        FlowNodeEntry{NodeId{3}, &call},
        FlowNodeEntry{NodeId{1}, &function},
        FlowNodeEntry{NodeId{2}, &callee},
        FlowNodeEntry{NodeId{4}, &wait}
    };
    auto edit = FlowGraphEdit::prepare(graph, {.insert = entries});
    require(edit.has_value());
    edit->commit();
    const auto entry = execution(graph, NodeId{1}, graph::EPinDirection::OUTPUT);
    const auto called_entry = execution(graph, NodeId{2}, graph::EPinDirection::OUTPUT);
    require(graph.connect(entry, execution(graph, NodeId{3}, graph::EPinDirection::INPUT)).has_value());
    require(graph.connect(called_entry, execution(graph, NodeId{4}, graph::EPinDirection::INPUT)).has_value());
    auto analyzed = FlowAnalysis::create(graph, {.script_abilities = abilities.view()});
    require(analyzed.has_value());
    require(analyzed->firstSuspensionFrom(entry) == NodeId{4});
    require(analyzed->firstSuspensionFrom(called_entry) == NodeId{4});
    require(analyzed->suspensionBetween(entry, NodeId{3}) == NodeId{4});
    require(analyzed->abilityRequirements().size() == 1);
    require(analyzed->abilityRequirements()[0].expected_schema_hash == 17);
    require(!graph.removeNode(NodeId{2}));
    const auto missing_catalog = FlowAnalysis::create(graph);
    require(!missing_catalog && missing_catalog.error().code == EFlowForgeError::UNKNOWN_SCRIPT_ABILITY_CONTRACT);
    require(missing_catalog.error().node_id == 4);
    require(missing_catalog.error().message == "the Script Ability contract is not present in the supplied catalog");

    ability.schema_hash = 18;
    ScriptAbilityNodeCatalog changed;
    require(changed.add({{&ability, 1}}).has_value());
    const auto mismatched = FlowAnalysis::create(graph, {.script_abilities = changed.view()});
    require(!mismatched && mismatched.error().code == EFlowForgeError::SCRIPT_ABILITY_SCHEMA_MISMATCH);
    require(mismatched.error().node_id == 4);

    // Analysis owns its facts after the analyzed authoring graph has been destroyed.
    graph = {};
    require(analyzed->firstSuspensionFrom(entry) == NodeId{4});
    require(analyzed->abilityRequirements()[0].contract.name() == "test.ability");
    std::puts(
        "PASS: real plain-store analysis, transitive suspension, reference guard, catalog diagnostics and lifetime"
    );
}

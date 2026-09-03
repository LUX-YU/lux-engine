#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityNode.hpp>

#include <array>
#include <cassert>
#include <cstdint>

int main()
{
    using namespace lux;
    static constexpr auto input = script::makeScriptAbilityValue<std::int32_t>(
        script::EScriptAbilityValueLifetime::OWNED_VALUE
    );
    static constexpr auto output = script::makeScriptAbilityValue<std::int32_t>(
        script::EScriptAbilityValueLifetime::OWNED_VALUE
    );
    static constexpr std::array parameters{script::ScriptAbilityParameterDescription{"value", input}};
    static constexpr std::array results{output};
    static constexpr flowforge::ScriptAbilityNodeDescription read{
        script::ScriptApiContractIdView{"test.Value"},
        script::ScriptApiMethodIdView{"read"},
        "Test Value",
        "Read",
        1U,
        0x11223344U,
        script::EScriptAbilityReceiverKind::PROVIDER_INSTANCE,
        script::EScriptApiMethodKind::QUERY,
        parameters,
        results
    };
    static constexpr std::array descriptions{read};

    flowforge::ScriptAbilityNodeCatalog catalog;
    assert(catalog.add({descriptions}));
    assert(catalog.view().find(read.contract, read.method) == &catalog.view().nodes().front());
    assert(!catalog.add({descriptions}));

    auto node = std::make_unique<flowforge::ScriptAbilityNode>(1U, read);
    assert(node->operation() == flowforge::ENodeOperation::SCRIPT_ABILITY_CALL);
    assert(node->contract() == read.contract);
    assert(node->method() == read.method);
    assert(node->expectedSchemaHash() == read.schema_hash);
    assert(node->parameterPins().size() == 1U);
    assert(node->resultPins().size() == 1U);
    assert(node->parameterPins().front()->info().type->hash == input.type_id);
    assert(node->resultPins().front()->info().type->hash == output.type_id);
    assert(node->inPins().size() == 2U);
    assert(node->outPins().size() == 2U);

    flowforge::FlowGraph graph;
    const auto index = graph.addNodes(std::move(node));
    assert(graph.hasNode(index));
    const auto* stored = dynamic_cast<const flowforge::ScriptAbilityNode*>(graph.getNode(index).node.get());
    assert(stored != nullptr);
    assert(stored->contract() == read.contract);
    return 0;
}

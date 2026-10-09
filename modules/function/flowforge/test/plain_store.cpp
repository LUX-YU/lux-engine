#include <lux/engine/flowforge/ScalarNodes.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/meta/Meta.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace
{
    using namespace lux;
    using namespace lux::flowforge;

    void require(bool condition, std::source_location location = std::source_location::current()) noexcept
    {
        if (!condition)
        {
            std::fprintf(stderr, "%s:%u\n", location.file_name(), location.line());
            std::abort();
        }
    }

    FlowNode addNode(const FlowNodeCatalog& catalog) noexcept
    {
        auto definition = catalog.find(graph::nodeTypeId("lux.flow.add"));
        require(definition != nullptr);
        auto payload = definition->create();
        require(payload.has_value());
        auto node = createFlowNode(std::move(definition), std::move(*payload));
        require(node.has_value());
        return std::move(*node);
    }

    PinId pinAt(const FlowGraph& graph, NodeId node, graph::EPinDirection direction, std::size_t ordinal = 0) noexcept
    {
        for (const auto& pin : graph.topology().pins())
        {
            if (pin.owner == node && pin.direction == direction)
            {
                if (ordinal == 0)
                {
                    return pin.id;
                }
                --ordinal;
            }
        }
        return {};
    }

    struct ReferencePayload final
    {
        NodeId target;
        int signature{7};
        bool reject_clone{};
    };

    FlowForgeResult<std::unique_ptr<ReferencePayload>> cloneReference(const ReferencePayload& value) noexcept
    {
        if (value.reject_clone)
        {
            return cxx::unexpected(FlowForgeFailure{EFlowForgeError::GRAPH_INVALID, "reference clone refused", 0, 83});
        }
        return std::make_unique<ReferencePayload>(value);
    }

    FlowNodeRegistration referenceRegistration() noexcept
    {
        FlowNodeRegistration value;
        value.identity = {graph::nodeTypeId("tests.flow.reference"), "tests.flow.reference", 1};
        value.payload_type = cxx::typeToken<ReferencePayload>();
        value.create = [](const object::CodeLease& code) noexcept
        { return FlowNodePayload::make<ReferencePayload, cloneReference>(code); };
        value.describe_pins = [](const FlowNodePayload&) noexcept -> FlowNodeRegistration::PinResult
        {
            return std::vector<FlowPinDeclaration>{
                {{1}, "input", graph::EPinDirection::INPUT, &meta::ref_type_of_v<int>},
                {{2}, "output", graph::EPinDirection::OUTPUT, &meta::ref_type_of_v<int>}
            };
        };
        value.validate = [](const FlowNodePayload& payload) noexcept -> FlowForgeResult<void>
        {
            if (payload.get<ReferencePayload>()->signature == 0)
            {
                return cxx::unexpected(FlowForgeFailure{EFlowForgeError::GRAPH_INVALID, "invalid reference signature"});
            }
            return {};
        };
        value.compile = [](const FlowNodePayload&,
                           std::span<const FlowValue> inputs,
                           FlowValueCompiler&) noexcept -> FlowNodeRegistration::ValueResult
        { return std::vector<FlowValue>{inputs.front()}; };
        value.validate_references = [](const FlowNodePayload& payload,
                                       FlowReferenceView view) noexcept -> FlowForgeResult<void>
        {
            const auto& reference = *payload.get<ReferencePayload>();
            if (!reference.target.valid())
            {
                return {};
            }
            const auto* target = view.node(reference.target);
            const auto* signature = target ? target->payload.get<ReferencePayload>() : nullptr;
            const bool is_valid = signature && signature->signature == reference.signature;
            if (!is_valid)
            {
                return cxx::unexpected(FlowForgeFailure{EFlowForgeError::GRAPH_INVALID, "reference signature mismatch"}
                );
            }
            return {};
        };
        return value;
    }

    FlowNode referenceNode(const FlowNodeCatalog& catalog, NodeId target, int signature) noexcept
    {
        auto definition = catalog.find(graph::nodeTypeId("tests.flow.reference"));
        auto payload = definition->create();
        require(payload.has_value());
        *payload->get<ReferencePayload>() = {target, signature};
        auto result = createFlowNode(std::move(definition), std::move(*payload));
        require(result.has_value());
        return std::move(*result);
    }

    void candidateReferences(FlowNodeCatalog& catalog) noexcept
    {
        const auto registration = referenceRegistration();
        require(catalog.add({&registration, 1}).has_value());
        FlowGraph graph;
        auto definition = referenceNode(catalog, {}, 7);
        auto caller = referenceNode(catalog, NodeId{17}, 7);
        const std::array entries{FlowNodeEntry{NodeId{19}, &caller, {}}, FlowNodeEntry{NodeId{17}, &definition, {}}};
        FlowGraphChange change;
        change.insert = entries;
        auto initial = FlowGraphEdit::prepare(graph, change);
        require(initial.has_value());
        require(initial->insertedNodes()[0].id == NodeId{19});
        initial->commit();
        const auto* original = graph.node(NodeId{17});
        require(original && graph.node(NodeId{19}));
        require(!graph.removeNode(NodeId{17}));
        require(graph.node(NodeId{17}) == original);

        auto replacement = referenceNode(catalog, {}, 8);
        const FlowNodeEntry replace{NodeId{17}, &replacement, {}};
        const NodeId erase{17};
        change = {};
        change.insert = {&replace, 1};
        change.erase = {&erase, 1};
        auto mismatch = FlowGraphEdit::prepare(graph, change);
        require(!mismatch && graph.node(erase) == original);
        const auto* error = std::get_if<FlowForgeFailure>(&mismatch.error());
        require(error && error->message == "reference signature mismatch" && error->node_id == 19);
        replacement.payload.get<ReferencePayload>()->signature = 7;
        auto accepted = FlowGraphEdit::prepare(graph, change);
        require(accepted.has_value() && graph.node(erase) == original);
        accepted->commit();
        require(graph.node(erase) != original);
        auto removed = accepted->takeRemoved();
        require(removed.size() == 1 && removed[0].value.definition != nullptr);

        replacement.payload.get<ReferencePayload>()->reject_clone = true;
        const auto* current = graph.node(erase);
        auto clone_failed = FlowGraphEdit::prepare(graph, change);
        require(!clone_failed && graph.node(erase) == current);
        error = std::get_if<FlowForgeFailure>(&clone_failed.error());
        require(error && error->message == "reference clone refused" && error->node_id == 17 && error->pin_id == 83);

        const std::array erase_both{NodeId{17}, NodeId{19}};
        change = {};
        change.erase = erase_both;
        auto clearing = FlowGraphEdit::prepare(graph, change);
        require(clearing.has_value());
        clearing->commit();
        require(graph.topology().nodes().empty());
    }
} // namespace

int main()
{
    static_assert(!std::is_polymorphic_v<FlowNode>);
    static_assert(!std::is_polymorphic_v<FlowPinPayload>);
    static_assert(!std::is_copy_constructible_v<FlowPinPayload>);
    static_assert(std::is_nothrow_move_constructible_v<FlowPinPayload>);
    meta::meta_module_init();
    FlowNodeCatalog catalog;
    require(catalog.add(scalarNodeRegistrations()).has_value());
    candidateReferences(catalog);
    FlowGraph graph;
    auto first = graph.addNode(addNode(catalog));
    auto second = graph.addNode(addNode(catalog));
    require(first.has_value() && second.has_value());
    const auto output = pinAt(graph, *first, graph::EPinDirection::OUTPUT);
    const auto input = pinAt(graph, *second, graph::EPinDirection::INPUT);
    require(output.valid() && input.valid());
    require(graph.pin(input)->setDefault(meta::RuntimeObject{37}));
    require(graph.connect(output, input).has_value());
    const graph::GraphLayoutEntry position{*first, {4, 11, true}};
    FlowGraphChange placement;
    placement.place = {&position, 1};
    auto placed = FlowGraphEdit::prepare(graph, placement);
    require(placed.has_value());
    placed->commit();

    auto snapshot = graph.extractNode(*first);
    require(snapshot.has_value());
    require(graph.node(*first) == nullptr && graph.pin(output) == nullptr);
    require(snapshot->links.size() == 1 && snapshot->layout == position.layout);
    require(graph.topology().links().empty());
    const FlowNodeEntry restore{snapshot->id, &snapshot->value, snapshot->pins};
    FlowGraphChange restored;
    restored.insert = {&restore, 1};
    restored.connect = snapshot->links;
    restored.place = {&position, 1};
    auto prepared = FlowGraphEdit::prepare(graph, restored);
    require(prepared.has_value());
    require(graph.node(*first) == nullptr && graph.topology().links().empty());
    require(snapshot->value.definition != nullptr);
    prepared->commit();
    require(graph.node(*first) != nullptr && graph.pin(output) != nullptr);
    require(graph.topology().findLink(output, input) != nullptr);
    require(graph.pin(input)->default_value.get<int>() == 37);
    require(graph.layout().find(*first) && *graph.layout().find(*first) == position.layout);

    FlowGraph moved(std::move(graph));
    require(moved.pin(input)->default_value.get<int>() == 37);
    require(moved.node(*first) != nullptr);
    graph = std::move(moved);
    require(graph.topology().findLink(output, input) != nullptr);

    // Restore the maximum valid PinId, then attempt a mixed remove/recreate with a fresh pin.
    FlowGraph exhausted;
    auto seed = exhausted.addNode(addNode(catalog));
    require(seed.has_value());
    auto maximum = exhausted.extractNode(*seed);
    require(maximum.has_value());
    maximum->pins.back().record.id = PinId{UINT64_MAX};
    const FlowNodeEntry maximum_entry{maximum->id, &maximum->value, maximum->pins};
    FlowGraphChange maximum_change;
    maximum_change.insert = {&maximum_entry, 1};
    auto maximum_edit = FlowGraphEdit::prepare(exhausted, maximum_change);
    require(maximum_edit.has_value());
    maximum_edit->commit();
    const auto* original_node = exhausted.node(*seed);
    const auto* original_pin = exhausted.pin(PinId{UINT64_MAX});
    maximum->pins.back().record.id = {};
    maximum_change.erase = {&*seed, 1};
    auto refused = FlowGraphEdit::prepare(exhausted, maximum_change);
    require(!refused);
    const auto* error = std::get_if<graph::GraphTopologyFailure>(&refused.error());
    require(error && error->code == graph::EGraphTopologyError::ID_EXHAUSTED);
    require(exhausted.node(*seed) == original_node && exhausted.pin(PinId{UINT64_MAX}) == original_pin);
    require(exhausted.topology().nodes().size() == 1 && exhausted.topology().pins().size() == 3);
    require(maximum->value.definition != nullptr && maximum->pins.back().record.id == PinId{});
    std::puts("PASS plain stores: topology, payload lookup, prepare/commit, restore, layout, move, maximum-ID refusal");
}

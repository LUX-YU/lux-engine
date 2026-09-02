#ifdef NDEBUG
#undef NDEBUG
#endif

#include <lux/engine/function/graph/GraphLayout.hpp>
#include <lux/engine/function/graph/GraphTopology.hpp>

#include <cassert>

int main()
{
    using namespace lux::graph;

    GraphTopology topology;
    assert(!topology.addNode({}));
    const auto first = topology.addNode(NodeTypeId{1U});
    const auto second = topology.addNode(NodeTypeId{2U});
    assert(first && second && *first != *second);
    assert(topology.nodes().size() == 2U);
    assert(!topology.insertNode(NodeRecord{*first, NodeTypeId{3U}}));
    const auto invalid_semantic = topology.addPin(*first, EPinDirection::OUTPUT, 1U, {});
    assert(!invalid_semantic && invalid_semantic.error().code == EGraphTopologyError::INVALID_SEMANTIC);

    const auto output = topology.addPin(
        *first,
        EPinDirection::OUTPUT,
        kUnlimitedFan,
        PinSemanticId{1U}
    );
    const auto input = topology.addPin(*second, EPinDirection::INPUT, 1U, PinSemanticId{2U});
    const auto other_input = topology.addPin(*second, EPinDirection::INPUT, 1U, PinSemanticId{3U});
    assert(output && input && other_input);
    assert(topology.connect(*output, *input));
    assert(!topology.connect(*output, *input));
    assert(!topology.connect(*input, *output));
    assert((topology.incoming(*input) == LinkRecord{*output, *input}));

    const auto replacement_output = topology.addPin(
        *first,
        EPinDirection::OUTPUT,
        kUnlimitedFan,
        PinSemanticId{4U}
    );
    assert(replacement_output);
    const auto full = topology.connect(*replacement_output, *input);
    assert(!full && full.error().code == EGraphTopologyError::FAN_CAP_EXCEEDED);

    auto detached_pin = topology.detachPin(*input);
    assert(detached_pin && detached_pin->links.size() == 1U);
    assert(topology.findPin(*input) == nullptr);
    assert(topology.links().empty());
    assert(topology.restorePin(std::move(*detached_pin)));
    assert(topology.findLink(*output, *input) != nullptr);

    auto detached_node = topology.detachNode(*first);
    assert(detached_node && detached_node->pins.size() == 2U && detached_node->links.size() == 1U);
    assert(topology.findNode(*first) == nullptr);
    assert(topology.findPin(*output) == nullptr);
    assert(topology.links().empty());
    assert(topology.restoreNode(std::move(*detached_node)));
    assert(topology.findNode(*first) != nullptr);
    assert(topology.findLink(*output, *input) != nullptr);

    auto conflicting = topology.detachNode(*first);
    assert(conflicting);
    assert(topology.insertNode(NodeRecord{*first, NodeTypeId{9U}}));
    const auto failed_restore = topology.restoreNode(std::move(*conflicting));
    assert(!failed_restore && failed_restore.error().code == EGraphTopologyError::DUPLICATE_NODE);
    assert(topology.findNode(*first)->type == NodeTypeId{9U});

    GraphLayout layout;
    assert(layout.set(*first, GraphNodeLayout{10.0F, 20.0F, true}));
    assert(layout.find(*first) != nullptr);
    assert(layout.find(*first)->x == 10.0F);
    auto clone = layout;
    assert(clone.find(*first) != nullptr);
    assert(clone.erase(*first));
    assert(clone.find(*first) == nullptr);
    assert(layout.find(*first) != nullptr);
    return 0;
}

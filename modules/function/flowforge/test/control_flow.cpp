#include <lux/engine/flowforge/ControlNodes.hpp>
#include <lux/engine/flowforge/FlowControlFlow.hpp>
#include <lux/engine/flowforge/graph/FlowSource.hpp>

#include <array>
#include <unordered_map>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <utility>

namespace
{
    using namespace lux::flowforge;

    void require(bool value, std::source_location at = std::source_location::current()) noexcept
    {
        if (!value)
        {
            std::fprintf(stderr, "Flow control query failed at %u\n", at.line());
            std::abort();
        }
    }

    NodeId add(FlowGraph& graph, std::string_view name, std::size_t extra = 0)
    {
        FlowNodeCatalog catalog;
        require(catalog.add(controlNodeRegistrations()).has_value());
        auto definition = catalog.find(lux::graph::nodeTypeId(name));
        auto payload = definition->create();
        require(payload.has_value());
        if (auto* sequence = payload->get<SequencePayload>())
        {
            sequence->additional_outputs = extra;
        }
        auto node = createFlowNode(definition, std::move(*payload));
        require(node.has_value());
        auto id = graph.addNode(std::move(*node));
        require(id.has_value());
        return *id;
    }

    PinId execution(const FlowGraph& graph, NodeId id, bool output, std::size_t ordinal = 0)
    {
        const auto* node = graph.node(id);
        if (!node)
        {
            return {};
        }
        const auto schema = node->definition->describePins(node->payload);
        require(schema.has_value());
        const auto direction = output ? lux::graph::EPinDirection::OUTPUT : lux::graph::EPinDirection::INPUT;
        for (const auto& pin : *schema)
        {
            const bool matches = pin.role == EFlowPinRole::EXECUTION && pin.direction == direction;
            if (matches && ordinal-- == 0)
            {
                return graph.pinId(id, pin.semantic);
            }
        }
        return {};
    }

    NodeId merge(const FlowGraph& graph, NodeId id)
    {
        return findBranchMerge(graph, id, execution(graph, id, true), execution(graph, id, true, 1));
    }

    void link(FlowGraph& graph, PinId from, PinId to)
    {
        require(graph.connect(from, to).has_value());
    }

    void diamond()
    {
        FlowGraph graph;
        auto branch = add(graph, "lux.flow.branch");
        auto left = add(graph, "lux.flow.sequence");
        auto right = add(graph, "lux.flow.sequence");
        auto end = add(graph, "lux.flow.return");
        require(!merge(graph, branch).valid());
        require(reachableExecution(graph, {}).empty());
        require(reachableExecution(graph, execution(graph, branch, false)).empty());
        require(!merge(graph, {}).valid());
        require(!merge(graph, left).valid());
        link(graph, execution(graph, branch, true), execution(graph, left, false));
        link(graph, execution(graph, branch, true, 1), execution(graph, right, false));
        link(graph, execution(graph, left, true), execution(graph, end, false));
        require(!merge(graph, branch).valid());
        link(graph, execution(graph, right, true), execution(graph, end, false));
        require(merge(graph, branch) == end);
        const auto reachable = reachableExecution(graph, execution(graph, branch, true));
        require(reachable == std::vector<NodeId>{left, end});
        require(graph.disconnect(execution(graph, right, true), execution(graph, end, false)).has_value());
        require(!merge(graph, branch).valid());
        // Earlier query results are owned IDs, not references into nodes or graph storage.
        require(graph.removeNode(left).has_value());
        require(reachable.size() == 2 && reachable.front().valid());
    }

    void orderedTieAndCycle()
    {
        FlowGraph graph;
        auto branch = add(graph, "lux.flow.branch");
        auto left = add(graph, "lux.flow.sequence", 1);
        auto right = add(graph, "lux.flow.sequence", 1);
        auto second = add(graph, "lux.flow.return");
        auto first = add(graph, "lux.flow.return");
        auto left_second = execution(graph, left, true, 1);
        auto right_second = execution(graph, right, true, 1);
        require(left_second.valid() && right_second.valid());
        link(graph, execution(graph, branch, true), execution(graph, left, false));
        link(graph, execution(graph, branch, true, 1), execution(graph, right, false));
        link(graph, execution(graph, left, true), execution(graph, first, false));
        link(graph, left_second, execution(graph, second, false));
        link(graph, execution(graph, right, true), execution(graph, second, false));
        link(graph, right_second, execution(graph, first, false));
        require(merge(graph, branch) == first);

        // Persisted pin numbers are identities, not execution ordering. Restore the same
        // graph with reversed numbers so numeric topology order disagrees with its schema.
        const lux::asset::AssetId asset{std::array<std::uint8_t, 16>{1}};
        auto source = captureFlowSource(asset, "ordered", graph);
        require(source.has_value());
        std::unordered_map<PinId, PinId> remap;
        std::uint64_t number = 1000;
        for (auto& node : source->nodes)
        {
            for (auto* pins : {&node.inputs, &node.outputs})
            {
                for (auto& pin : *pins)
                {
                    const PinId replacement{number--};
                    remap.emplace(pin.id, replacement);
                    pin.id = replacement;
                }
            }
        }
        for (auto& edge : source->links)
        {
            edge.from = remap.at(edge.from);
            edge.to = remap.at(edge.to);
        }
        std::ranges::sort(
            source->links,
            [](const auto& a, const auto& b) noexcept { return a.from == b.from ? a.to < b.to : a.from < b.from; }
        );
        auto restored = materializeFlowSource(*source);
        require(restored.has_value());
        require(merge(*restored, branch) == first);
        require(
            reachableExecution(*restored, execution(*restored, branch, true)) ==
            std::vector<NodeId>{left, first, second}
        );
        const auto recaptured = captureFlowSource(asset, "ordered", *restored);
        require(recaptured.has_value() && *recaptured == *source);

        FlowGraph cyclic;
        auto start = add(cyclic, "lux.flow.branch");
        auto loop = add(cyclic, "lux.flow.sequence");
        auto exit = add(cyclic, "lux.flow.return");
        link(cyclic, execution(cyclic, start, true), execution(cyclic, loop, false));
        link(cyclic, execution(cyclic, loop, true), execution(cyclic, start, false));
        link(cyclic, execution(cyclic, start, true, 1), execution(cyclic, exit, false));
        const auto reached = reachableExecution(cyclic, execution(cyclic, start, true));
        require(reached.size() == 3);
        require(std::ranges::count(reached, start) == 1);
        require(merge(cyclic, start) == exit);
    }

    void deepChain()
    {
        FlowGraph graph;
        auto first = add(graph, "lux.flow.sequence");
        auto tail = first;
        for (unsigned index = 0; index != 2048; ++index)
        {
            auto next = add(graph, "lux.flow.sequence");
            link(graph, execution(graph, tail, true), execution(graph, next, false));
            tail = next;
        }
        const auto reached = reachableExecution(graph, execution(graph, first, true));
        require(reached.size() == 2048 && reached.back() == tail);
    }
} // namespace

int main()
{
    diamond();
    orderedTieAndCycle();
    deepChain();
}

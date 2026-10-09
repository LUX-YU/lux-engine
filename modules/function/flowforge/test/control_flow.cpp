#include <lux/engine/flowforge/FlowControlFlow.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>

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

    template <class T, class... Args> T& add(FlowGraph& graph, Args&&... args)
    {
        auto candidate = std::make_unique<T>(std::forward<Args>(args)...);
        auto& result = *candidate;
        require(graph.addNode(std::move(candidate)).valid());
        return result;
    }

    void link(FlowGraph& graph, const Pin& from, const Pin& to)
    {
        auto* output = graph.findPin(graph.pinId(&from));
        auto* input = graph.findPin(graph.pinId(&to));
        require(graph.connect(*output, *input) == ELinkError::SUCCESS);
    }

    void diamond()
    {
        FlowGraph graph;
        auto& branch = add<BranchNode>(graph);
        auto& left = add<SequenceNode>(graph);
        auto& right = add<SequenceNode>(graph);
        auto& end = add<ReturnNode>(graph);
        require(!findBranchMerge(graph, graph.nodeId(&branch)).valid());
        require(reachableExecution(graph, {}).empty());
        require(reachableExecution(graph, graph.pinId(&branch.execInPin())).empty());
        require(!findBranchMerge(graph, {}).valid());
        require(!findBranchMerge(graph, graph.nodeId(&left)).valid());
        link(graph, branch.execOutPinUp(), left.execInPin());
        link(graph, branch.execOutPinDown(), right.execInPin());
        link(graph, left.execOutPin(), end.execInPin());
        require(!findBranchMerge(graph, graph.nodeId(&branch)).valid());
        link(graph, right.execOutPin(), end.execInPin());
        require(findBranchMerge(graph, graph.nodeId(&branch)) == graph.nodeId(&end));
        const auto reachable = reachableExecution(graph, graph.pinId(&branch.execOutPinUp()));
        require(reachable == std::vector<NodeId>{graph.nodeId(&left), graph.nodeId(&end)});
        require(graph.disconnect(right.execOutPin(), end.execInPin()) == ELinkError::UNLINKED);
        require(!findBranchMerge(graph, graph.nodeId(&branch)).valid());
        // Earlier query results are owned IDs, not references into nodes or graph storage.
        require(graph.removeNode(graph.nodeId(&left)));
        require(reachable.size() == 2 && reachable.front().valid());
    }

    void orderedTieAndCycle()
    {
        FlowGraph graph;
        auto& branch = add<BranchNode>(graph);
        auto& left = add<SequenceNode>(graph, SequenceSchema{1});
        auto& right = add<SequenceNode>(graph, SequenceSchema{1});
        auto& second = add<ReturnNode>(graph);
        auto& first = add<ReturnNode>(graph);
        auto* left_second = left.execOutPins().front().get();
        auto* right_second = right.execOutPins().front().get();
        require(left_second != nullptr && right_second != nullptr);
        link(graph, branch.execOutPinUp(), left.execInPin());
        link(graph, branch.execOutPinDown(), right.execInPin());
        link(graph, left.execOutPin(), first.execInPin());
        link(graph, *left_second, second.execInPin());
        link(graph, right.execOutPin(), second.execInPin());
        link(graph, *right_second, first.execInPin());
        require(findBranchMerge(graph, graph.nodeId(&branch)) == graph.nodeId(&first));

        FlowGraph cyclic;
        auto& start = add<BranchNode>(cyclic);
        auto& loop = add<SequenceNode>(cyclic);
        auto& exit = add<ReturnNode>(cyclic);
        link(cyclic, start.execOutPinUp(), loop.execInPin());
        link(cyclic, loop.execOutPin(), start.execInPin());
        link(cyclic, start.execOutPinDown(), exit.execInPin());
        const auto reached = reachableExecution(cyclic, cyclic.pinId(&start.execOutPinUp()));
        require(reached.size() == 3);
        require(std::ranges::count(reached, cyclic.nodeId(&start)) == 1);
        require(findBranchMerge(cyclic, cyclic.nodeId(&start)) == cyclic.nodeId(&exit));
    }

    void deepChain()
    {
        FlowGraph graph;
        auto& first = add<SequenceNode>(graph);
        auto* tail = &first;
        for (unsigned index = 0; index != 2048; ++index)
        {
            auto& next = add<SequenceNode>(graph);
            link(graph, tail->execOutPin(), next.execInPin());
            tail = &next;
        }
        const auto reached = reachableExecution(graph, graph.pinId(&first.execOutPin()));
        require(reached.size() == 2048 && reached.back() == graph.nodeId(tail));
    }
} // namespace

int main()
{
    diamond();
    orderedTieAndCycle();
    deepChain();
}

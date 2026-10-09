#include <lux/engine/flowforge/FlowControlFlow.hpp>

#include <lux/engine/flowforge/detail/ExecutionTraversal.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>

#include <array>
#include <unordered_set>

namespace lux::flowforge
{
    namespace
    {
        [[nodiscard]] NodeId executionStart(const FlowGraph& graph, PinId id) noexcept
        {
            const auto* pin = graph.findPin(id);
            const bool is_execution_output = pin != nullptr && pin->kind() == EPinKind::EXEC_OUT;
            if (!is_execution_output)
            {
                return {};
            }
            const auto* next = static_cast<const ExecOutPin*>(pin)->nextPin();
            return next != nullptr ? graph.nodeId(next->node()) : NodeId{};
        }

        template <class Emit> void successors(const FlowGraph& graph, NodeId id, Emit&& emit) noexcept
        {
            const auto& node = *graph.findNodeById(id);
            for (const auto* pin : node.outPins())
            {
                if (pin->kind() != EPinKind::EXEC_OUT)
                {
                    continue;
                }
                if (const auto* next = static_cast<const ExecOutPin*>(pin)->nextPin())
                {
                    emit(graph.nodeId(next->node()));
                }
            }
        }
    } // namespace

    std::vector<NodeId> reachableExecution(const FlowGraph& graph, PinId start) noexcept
    {
        const std::array roots{executionStart(graph, start)};
        std::vector<NodeId> result;
        detail::visitExecution(
            roots,
            [&](NodeId id, auto&& emit) noexcept { successors(graph, id, emit); },
            [&](NodeId id) noexcept
            {
                result.push_back(id);
                return true;
            }
        );
        return result;
    }

    NodeId findBranchMerge(const FlowGraph& graph, NodeId branch) noexcept
    {
        const auto* node = graph.findNodeById(branch);
        const bool is_branch = node != nullptr && node->operation() == ENodeOperation::BRANCH;
        if (!is_branch)
        {
            return {};
        }
        const auto& control = static_cast<const BranchNode&>(*node);
        const auto up = graph.pinId(&control.execOutPinUp());
        const auto down = graph.pinId(&control.execOutPinDown());
        const auto up_reachable = reachableExecution(graph, up);
        const auto down_reachable = reachableExecution(graph, down);
        const std::unordered_set<NodeId> down_set{down_reachable.begin(), down_reachable.end()};
        std::unordered_set<NodeId> common;
        for (const auto id : up_reachable)
        {
            if (down_set.contains(id))
            {
                common.insert(id);
            }
        }
        if (common.empty())
        {
            return {};
        }
        NodeId result;
        const std::array roots{executionStart(graph, up), executionStart(graph, down)};
        detail::visitExecution(
            roots,
            [&](NodeId id, auto&& emit) noexcept { successors(graph, id, emit); },
            [&](NodeId id) noexcept
            {
                if (common.contains(id))
                {
                    result = id;
                    return false;
                }
                return true;
            },
            branch
        );
        return result;
    }
} // namespace lux::flowforge

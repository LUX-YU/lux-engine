#include <lux/engine/flowforge/FlowControlFlow.hpp>

#include <lux/engine/flowforge/detail/ExecutionTraversal.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>

#include <array>
#include <unordered_set>

namespace lux::flowforge
{
    namespace
    {
        [[nodiscard]] NodeId executionStart(const FlowGraph& graph, PinId id) noexcept
        {
            const auto* pin = graph.pin(id);
            const auto* record = graph.topology().findPin(id);
            const bool has_pin = pin && record;
            const bool is_execution_output =
                has_pin && pin->role == EFlowPinRole::EXECUTION && record->direction == graph::EPinDirection::OUTPUT;
            if (!is_execution_output)
            {
                return {};
            }
            for (const auto& link : graph.topology().links())
            {
                if (link.from == id)
                {
                    const auto* input = graph.topology().findPin(link.to);
                    return input ? input->owner : NodeId{};
                }
            }
            return {};
        }

        template <class Emit> void successors(const FlowGraph& graph, NodeId id, Emit&& emit) noexcept
        {
            const auto* node = graph.node(id);
            if (!node)
            {
                return;
            }
            const auto schema = node->definition->describePins(node->payload);
            if (!schema)
            {
                return;
            }
            for (const auto& declaration : *schema)
            {
                const bool is_execution_output = declaration.direction == graph::EPinDirection::OUTPUT &&
                                                 declaration.role == EFlowPinRole::EXECUTION;
                if (!is_execution_output)
                {
                    continue;
                }
                const auto next = executionStart(graph, graph.pinId(id, declaration.semantic));
                if (next.valid())
                {
                    emit(next);
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

    NodeId findBranchMerge(const FlowGraph& graph, NodeId branch, PinId up, PinId down) noexcept
    {
        const auto is_leg = [&](PinId id) noexcept
        {
            const auto* record = graph.topology().findPin(id);
            const auto* payload = graph.pin(id);
            const bool has_pin = record && payload;
            return has_pin && record->owner == branch && record->direction == graph::EPinDirection::OUTPUT &&
                   payload->role == EFlowPinRole::EXECUTION;
        };
        const bool is_valid_branch = up != down && is_leg(up) && is_leg(down);
        if (!is_valid_branch)
        {
            return {};
        }
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

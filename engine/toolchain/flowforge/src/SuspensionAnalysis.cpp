#include <lux/engine/flowforge/compiler/SuspensionAnalysis.hpp>

#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/script/ScriptEventAwaitNode.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityNode.hpp>

#include <algorithm>
#include <memory>
#include <new>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace lux::flowforge
{
    namespace
    {
        [[nodiscard]] const Node* earlierWitness(
            const Node* left,
            const Node* right
        ) noexcept
        {
            if (left == nullptr)
                return right;
            if (right == nullptr)
                return left;
            return left->id().value <= right->id().value ? left : right;
        }

        template<class Callback>
        void visitDirectExecution(const ExecOutPin& start, Callback&& callback)
        {
            std::queue<const Node*> pending;
            std::unordered_set<const Node*> visited;
            if (const auto* next = start.nextPin())
                pending.push(next->node());

            while (!pending.empty())
            {
                const auto* node = pending.front();
                pending.pop();
                if (!visited.insert(node).second)
                    continue;

                callback(*node);
                for (const auto* pin : node->outPins())
                {
                    if (pin->kind() != EPinKind::EXEC_OUT)
                        continue;
                    if (const auto* next = static_cast<const ExecOutPin*>(pin)->nextPin())
                        pending.push(next->node());
                }
            }
        }
    }

    SuspensionAnalysis::SuspensionAnalysis(const FlowGraph& graph) noexcept : graph_(&graph)
    {
    }

    FlowForgeResult<SuspensionAnalysis> SuspensionAnalysis::create(const FlowGraph& graph) noexcept
    {
        try
        {
            SuspensionAnalysis analysis(graph);
            if (auto built = analysis.build(); !built)
                return lux::cxx::unexpected(std::move(built.error()));
            return analysis;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::ALLOCATION_FAILURE});
        }
    }

    FlowForgeResult<void> SuspensionAnalysis::build()
    {
        for (const auto& storage : graph_->nodes())
        {
            if (storage.node->operation() != ENodeOperation::FUNC_DEF_START)
                continue;
            const auto* function = static_cast<const FuncDefNode*>(storage.node.get());
            functions_.emplace(function, FunctionSummary{});
        }

        for (auto& [function, summary] : functions_)
        {
            visitDirectExecution(function->execOutPin(), [&](const Node& node) {
                if (node.operation() == ENodeOperation::SCRIPT_ABILITY_CALL)
                {
                    const auto* ability = static_cast<const ScriptAbilityNode*>(std::addressof(node));
                    if (ability->methodKind() == lux::script::EScriptApiMethodKind::ASYNC_OPERATION)
                        summary.direct_witness = earlierWitness(summary.direct_witness, ability);
                }
                else if (node.operation() == ENodeOperation::SCRIPT_EVENT_WAIT)
                {
                    summary.direct_witness = earlierWitness(summary.direct_witness, std::addressof(node));
                }
                else if (node.operation() == ENodeOperation::GRAPH_FUNC_CALL)
                {
                    const auto* callee = static_cast<const GraphFuncCallNode&>(node).callee();
                    if (callee != nullptr && std::ranges::find(summary.callees, callee) == summary.callees.end())
                        summary.callees.push_back(callee);
                }
            });

            std::ranges::sort(summary.callees, [](const auto* left, const auto* right) {
                return left->id().value < right->id().value;
            });
            for (const auto* callee : summary.callees)
            {
                if (!functions_.contains(callee))
                {
                    return lux::cxx::unexpected(FlowForgeFailure{
                        .code = EFlowForgeError::GRAPH_INVALID,
                        .message = "graph function call references a definition outside the graph",
                        .node_id = function->id().value
                    });
                }
            }
            summary.transitive_witness = summary.direct_witness;
        }

        bool changed{};
        do
        {
            changed = false;
            for (auto& [function, summary] : functions_)
            {
                static_cast<void>(function);
                auto* witness = summary.direct_witness;
                for (const auto* callee : summary.callees)
                    witness = earlierWitness(witness, functions_.at(callee).transitive_witness);
                if (witness != summary.transitive_witness)
                {
                    summary.transitive_witness = witness;
                    changed = true;
                }
            }
        } while (changed);

        return {};
    }

    const Node* SuspensionAnalysis::callWitness(const Node& node) const noexcept
    {
        if (node.operation() == ENodeOperation::SCRIPT_ABILITY_CALL)
        {
            const auto* ability = static_cast<const ScriptAbilityNode*>(std::addressof(node));
            return ability->methodKind() == lux::script::EScriptApiMethodKind::ASYNC_OPERATION ? ability : nullptr;
        }
        if (node.operation() != ENodeOperation::GRAPH_FUNC_CALL)
            return node.operation() == ENodeOperation::SCRIPT_EVENT_WAIT ? std::addressof(node) : nullptr;
        const auto* callee = static_cast<const GraphFuncCallNode&>(node).callee();
        const auto found = functions_.find(callee);
        return found == functions_.end() ? nullptr : found->second.transitive_witness;
    }

    const Node* SuspensionAnalysis::firstSuspensionFrom(const ExecOutPin& start) const
    {
        const Node* result{};
        visitDirectExecution(start, [&](const Node& node) {
            result = earlierWitness(result, callWitness(node));
        });
        return result;
    }

    const Node* SuspensionAnalysis::suspensionBetween(
        const ExecOutPin& start,
        const Node& target
    ) const
    {
        struct Visit final
        {
            const Node* node{};
            const Node* suspension{};
        };

        std::queue<Visit> pending;
        std::unordered_set<const Node*> visited_without_suspension;
        std::unordered_map<const Node*, const Node*> visited_with_suspension;
        const Node* result{};
        if (const auto* next = start.nextPin())
            pending.push({next->node(), nullptr});

        while (!pending.empty())
        {
            auto visit = pending.front();
            pending.pop();
            visit.suspension = earlierWitness(visit.suspension, callWitness(*visit.node));

            if (visit.suspension == nullptr)
            {
                if (!visited_without_suspension.insert(visit.node).second)
                    continue;
            }
            else
            {
                const auto [found, inserted] = visited_with_suspension.emplace(visit.node, visit.suspension);
                if (!inserted && found->second->id().value <= visit.suspension->id().value)
                    continue;
                if (!inserted)
                    found->second = visit.suspension;
            }

            if (visit.node == std::addressof(target))
            {
                result = earlierWitness(result, visit.suspension);
                continue;
            }

            for (const auto* pin : visit.node->outPins())
            {
                if (pin->kind() != EPinKind::EXEC_OUT)
                    continue;
                if (const auto* next = static_cast<const ExecOutPin*>(pin)->nextPin())
                    pending.push({next->node(), visit.suspension});
            }
        }
        return result;
    }
}

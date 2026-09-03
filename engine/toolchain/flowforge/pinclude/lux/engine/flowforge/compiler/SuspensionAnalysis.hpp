#pragma once

#include <lux/engine/flowforge/Compiler.hpp>

#include <unordered_map>
#include <vector>

namespace lux::flowforge
{
    class ExecOutPin;
    class FlowGraph;
    class FuncDefNode;
    class Node;
    class ScriptAbilityNode;

    class SuspensionAnalysis final
    {
    public:
        [[nodiscard]] static FlowForgeResult<SuspensionAnalysis> create(const FlowGraph& graph) noexcept;

        [[nodiscard]] const ScriptAbilityNode* firstSuspensionFrom(const ExecOutPin& start) const;
        [[nodiscard]] const ScriptAbilityNode* suspensionBetween(
            const ExecOutPin& start,
            const Node& target
        ) const;

    private:
        struct FunctionSummary final
        {
            const ScriptAbilityNode* direct_witness{};
            const ScriptAbilityNode* transitive_witness{};
            std::vector<const FuncDefNode*> callees;
        };

        explicit SuspensionAnalysis(const FlowGraph& graph) noexcept;

        [[nodiscard]] FlowForgeResult<void> build();
        [[nodiscard]] const ScriptAbilityNode* callWitness(const Node& node) const noexcept;

        const FlowGraph* graph_{};
        std::unordered_map<const FuncDefNode*, FunctionSummary> functions_;
    };
}

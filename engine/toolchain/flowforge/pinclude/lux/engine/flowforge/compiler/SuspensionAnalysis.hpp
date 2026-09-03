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

    class SuspensionAnalysis final
    {
    public:
        [[nodiscard]] static FlowForgeResult<SuspensionAnalysis> create(const FlowGraph& graph) noexcept;

        [[nodiscard]] const Node* firstSuspensionFrom(const ExecOutPin& start) const;
        [[nodiscard]] const Node* suspensionBetween(
            const ExecOutPin& start,
            const Node& target
        ) const;

    private:
        struct FunctionSummary final
        {
            const Node* direct_witness{};
            const Node* transitive_witness{};
            std::vector<const FuncDefNode*> callees;
        };

        explicit SuspensionAnalysis(const FlowGraph& graph) noexcept;

        [[nodiscard]] FlowForgeResult<void> build();
        [[nodiscard]] const Node* callWitness(const Node& node) const noexcept;

        const FlowGraph* graph_{};
        std::unordered_map<const FuncDefNode*, FunctionSummary> functions_;
    };
}

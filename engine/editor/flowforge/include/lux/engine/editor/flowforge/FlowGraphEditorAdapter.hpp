#pragma once

#include <lux/engine/editor/flowforge/visibility.h>
#include <lux/engine/editor/node_graph/GraphDocument.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>

#include <cstdint>
#include <vector>

namespace lux::editor::flow_graph
{
    enum class EFlowGraphNodeAction : std::uint64_t
    {
        ADD_SEQUENCE_OUTPUT = 1U,
        REMOVE_SEQUENCE_OUTPUT = 2U,
    };

    class LUX_EDITOR_FLOW_GRAPH_PUBLIC FlowGraphDocument final : public node_graph::IGraphDocument
    {
    public:
        explicit FlowGraphDocument(flowforge::FlowGraph& graph) noexcept;

        [[nodiscard]] graph::GraphTopology& topology() noexcept override;
        [[nodiscard]] const graph::GraphTopology& topology() const noexcept override;
        [[nodiscard]] graph::GraphLayout& layout() noexcept override;
        [[nodiscard]] const graph::GraphLayout& layout() const noexcept override;
        [[nodiscard]] graph::NodeId addNode(graph::NodeTypeId type) override;
        [[nodiscard]] node_graph::NodeCapture detachNode(graph::NodeId node) override;
        [[nodiscard]] bool attachNode(graph::NodeId original, node_graph::NodeCapture capture) override;
        [[nodiscard]] std::optional<node_graph::NodeActionJournal>
        invokeNodeAction(graph::NodeId node, std::uint64_t action) override;
        [[nodiscard]] bool restoreNodeAction(graph::NodeId node, node_graph::NodeCapture state) override;

        [[nodiscard]] flowforge::FlowGraph& source() noexcept;
        [[nodiscard]] const flowforge::FlowGraph& source() const noexcept;

    private:
        flowforge::FlowGraph* graph_{};
    };

    class LUX_EDITOR_FLOW_GRAPH_PUBLIC FlowGraphRules final : public node_graph::IGraphRules
    {
    public:
        explicit FlowGraphRules(const flowforge::FlowGraph& graph) noexcept;

        [[nodiscard]] bool canConnect(
            const node_graph::IGraphDocument& document,
            graph::PinId from,
            graph::PinId to
        ) const noexcept override;

    private:
        const flowforge::FlowGraph* graph_{};
    };

    class LUX_EDITOR_FLOW_GRAPH_PUBLIC FlowGraphPresentation final : public node_graph::IGraphPresentation
    {
    public:
        explicit FlowGraphPresentation(const flowforge::FlowGraph& graph);

        [[nodiscard]] node_graph::GraphNodePresentation node(graph::NodeId node) const noexcept override;
        [[nodiscard]] node_graph::GraphPinPresentation pin(graph::PinId pin) const noexcept override;
        [[nodiscard]] std::span<const node_graph::GraphPaletteEntry> palette() const noexcept override;

    private:
        const flowforge::FlowGraph* graph_{};
        std::vector<node_graph::GraphPaletteEntry> palette_;
    };
} // namespace lux::editor::flow_graph

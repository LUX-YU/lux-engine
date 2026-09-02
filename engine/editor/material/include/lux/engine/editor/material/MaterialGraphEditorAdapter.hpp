#pragma once

#include <lux/engine/editor/material/visibility.h>
#include <lux/engine/editor/node_graph/GraphDocument.hpp>
#include <lux/engine/material/graph/MaterialGraph.hpp>

#include <vector>

namespace lux::editor::material_graph
{
    class LUX_EDITOR_MATERIAL_GRAPH_PUBLIC MaterialGraphDocument final
        : public node_graph::IGraphDocument
    {
    public:
        explicit MaterialGraphDocument(material::MaterialGraph& graph) noexcept;

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

        [[nodiscard]] material::MaterialGraph& source() noexcept;
        [[nodiscard]] const material::MaterialGraph& source() const noexcept;

    private:
        material::MaterialGraph* graph_{};
    };

    class LUX_EDITOR_MATERIAL_GRAPH_PUBLIC MaterialGraphRules final : public node_graph::IGraphRules
    {
    public:
        explicit MaterialGraphRules(const material::MaterialGraph& graph) noexcept;

        [[nodiscard]] bool canConnect(
            const node_graph::IGraphDocument& document,
            graph::PinId from,
            graph::PinId to
        ) const noexcept override;

    private:
        const material::MaterialGraph* graph_{};
    };

    class LUX_EDITOR_MATERIAL_GRAPH_PUBLIC MaterialGraphPresentation final
        : public node_graph::IGraphPresentation
    {
    public:
        explicit MaterialGraphPresentation(const material::MaterialGraph& graph);

        [[nodiscard]] node_graph::GraphNodePresentation node(graph::NodeId node) const noexcept override;
        [[nodiscard]] node_graph::GraphPinPresentation pin(graph::PinId pin) const noexcept override;
        [[nodiscard]] std::span<const node_graph::GraphPaletteEntry> palette() const noexcept override;

    private:
        const material::MaterialGraph* graph_{};
        std::vector<node_graph::GraphPaletteEntry> palette_;
    };
} // namespace lux::editor::material_graph

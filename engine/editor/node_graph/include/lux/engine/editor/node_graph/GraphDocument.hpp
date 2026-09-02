#pragma once

#include <lux/engine/function/graph/GraphLayout.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace lux::editor::node_graph
{
    using NodeCapture = std::shared_ptr<void>;

    struct NodeActionJournal final
    {
        NodeCapture before;
        NodeCapture after;
    };

    class IGraphDocument
    {
    public:
        virtual ~IGraphDocument() = default;

        [[nodiscard]] virtual graph::GraphTopology& topology() noexcept = 0;
        [[nodiscard]] virtual const graph::GraphTopology& topology() const noexcept = 0;
        [[nodiscard]] virtual graph::GraphLayout& layout() noexcept = 0;
        [[nodiscard]] virtual const graph::GraphLayout& layout() const noexcept = 0;

        [[nodiscard]] virtual graph::NodeId addNode(graph::NodeTypeId type) = 0;
        [[nodiscard]] virtual NodeCapture detachNode(graph::NodeId node) = 0;
        [[nodiscard]] virtual bool attachNode(graph::NodeId original, NodeCapture capture) = 0;
        [[nodiscard]] virtual std::optional<NodeActionJournal>
        invokeNodeAction(graph::NodeId node, std::uint64_t action) = 0;
        [[nodiscard]] virtual bool restoreNodeAction(graph::NodeId node, NodeCapture state) = 0;
    };

    struct GraphPaletteEntry final
    {
        graph::NodeTypeId type;
        std::string display_name;
        std::string category;
    };

    struct GraphPinPresentation final
    {
        std::string_view label;
        std::uint32_t color{0xFFFFFFFFU};
    };

    struct GraphNodePresentation final
    {
        std::string_view title;
        std::uint32_t header_color{0xFF646464U};
    };

    class IGraphRules
    {
    public:
        virtual ~IGraphRules() = default;
        [[nodiscard]] virtual bool canConnect(
            const IGraphDocument& document,
            graph::PinId from,
            graph::PinId to
        ) const noexcept = 0;
    };

    class IGraphPresentation
    {
    public:
        virtual ~IGraphPresentation() = default;
        [[nodiscard]] virtual GraphNodePresentation node(graph::NodeId node) const noexcept = 0;
        [[nodiscard]] virtual GraphPinPresentation pin(graph::PinId pin) const noexcept = 0;
        [[nodiscard]] virtual std::span<const GraphPaletteEntry> palette() const noexcept = 0;
    };
} // namespace lux::editor::node_graph

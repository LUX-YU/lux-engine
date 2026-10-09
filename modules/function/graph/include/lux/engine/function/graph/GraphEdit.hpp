#pragma once

#include <lux/engine/function/graph/GraphLayout.hpp>

#include <optional>

namespace lux::graph
{
    // Exclusive synchronous borrow of the two published stores until commit or destruction.
    // Preparation only changes candidates and the source identity high-water marks. Issued identities
    // never recycle, including when a candidate is rejected or abandoned. Explicit restoration keeps IDs.
    // Commit swaps pure structural values without allocating or invoking domain callbacks.
    class LUX_FUNCTION_GRAPH_PUBLIC GraphEdit final
    {
    public:
        GraphEdit(GraphTopology&, GraphLayout&) noexcept;
        ~GraphEdit() = default;
        GraphEdit(GraphEdit&&) noexcept;
        GraphEdit(const GraphEdit&) = delete;
        GraphEdit& operator=(const GraphEdit&) = delete;
        GraphEdit& operator=(GraphEdit&&) = delete;

        [[nodiscard]] const GraphTopology& topology() const noexcept;
        [[nodiscard]] const GraphLayout& layout() const noexcept;

        [[nodiscard]] lux::cxx::expected<NodeId, GraphTopologyFailure> addNode(NodeTypeId) noexcept;
        [[nodiscard]] lux::cxx::expected<void, GraphTopologyFailure> insertNode(NodeRecord) noexcept;
        [[nodiscard]] lux::cxx::expected<DetachedNode, GraphTopologyFailure> detachNode(NodeId) noexcept;
        [[nodiscard]] lux::cxx::expected<void, GraphTopologyFailure> restoreNode(DetachedNode) noexcept;

        [[nodiscard]] lux::cxx::expected<PinId, GraphTopologyFailure> addPin(
            NodeId owner,
            EPinDirection,
            std::uint8_t fan_cap,
            PinSemanticId
        ) noexcept;
        [[nodiscard]] lux::cxx::expected<void, GraphTopologyFailure> insertPin(PinRecord) noexcept;
        [[nodiscard]] lux::cxx::expected<DetachedPin, GraphTopologyFailure> detachPin(PinId) noexcept;
        [[nodiscard]] lux::cxx::expected<void, GraphTopologyFailure> restorePin(DetachedPin) noexcept;

        [[nodiscard]] lux::cxx::expected<void, GraphTopologyFailure> connect(PinId from, PinId to) noexcept;
        [[nodiscard]] lux::cxx::expected<void, GraphTopologyFailure> disconnect(PinId from, PinId to) noexcept;
        [[nodiscard]] lux::cxx::expected<void, GraphTopologyFailure> place(NodeId, GraphNodeLayout) noexcept;
        [[nodiscard]] lux::cxx::expected<void, GraphTopologyFailure> unplace(NodeId) noexcept;

        void commit() noexcept;

    private:
        [[nodiscard]] GraphTopology& editTopology() noexcept;
        [[nodiscard]] GraphLayout& editLayout() noexcept;
        void preserveIssuedIds() noexcept;

        GraphTopology* target_topology_;
        GraphLayout* target_layout_;
        std::optional<GraphTopology> topology_;
        std::optional<GraphLayout> layout_;
    };
} // namespace lux::graph

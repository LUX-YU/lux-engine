#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/function/graph/GraphTypes.hpp>
#include <lux/engine/function/graph/visibility.h>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace lux::graph
{
    enum class EGraphTopologyError : std::uint8_t
    {
        INVALID_ID,
        INVALID_TYPE,
        INVALID_SEMANTIC,
        INVALID_DIRECTION,
        INVALID_FAN_CAP,
        DUPLICATE_NODE,
        DUPLICATE_PIN,
        DUPLICATE_LINK,
        UNKNOWN_NODE,
        UNKNOWN_PIN,
        UNKNOWN_LINK,
        DIRECTION_MISMATCH,
        FAN_CAP_EXCEEDED,
        ID_EXHAUSTED,
        ALLOCATION_FAILURE,
    };

    struct GraphTopologyFailure final
    {
        EGraphTopologyError code{EGraphTopologyError::INVALID_ID};
        NodeId node;
        PinId pin;
        PinId related_pin;
    };

    class LUX_FUNCTION_GRAPH_PUBLIC GraphTopology final
    {
    public:
        GraphTopology() = default;

        [[nodiscard]] lux::cxx::expected<NodeId, GraphTopologyFailure> addNode(NodeTypeId type) noexcept;
        [[nodiscard]] lux::cxx::expected<void, GraphTopologyFailure> insertNode(NodeRecord node) noexcept;
        [[nodiscard]] lux::cxx::expected<DetachedNode, GraphTopologyFailure> detachNode(NodeId node) noexcept;
        [[nodiscard]] lux::cxx::expected<void, GraphTopologyFailure> restoreNode(DetachedNode node) noexcept;

        [[nodiscard]] lux::cxx::expected<PinId, GraphTopologyFailure> addPin(
            NodeId owner,
            EPinDirection direction,
            std::uint8_t fan_cap,
            PinSemanticId semantic
        ) noexcept;
        [[nodiscard]] lux::cxx::expected<void, GraphTopologyFailure> insertPin(PinRecord pin) noexcept;
        [[nodiscard]] lux::cxx::expected<DetachedPin, GraphTopologyFailure> detachPin(PinId pin) noexcept;
        [[nodiscard]] lux::cxx::expected<void, GraphTopologyFailure> restorePin(DetachedPin pin) noexcept;

        [[nodiscard]] lux::cxx::expected<void, GraphTopologyFailure> connect(PinId from, PinId to) noexcept;
        [[nodiscard]] lux::cxx::expected<void, GraphTopologyFailure> disconnect(PinId from, PinId to) noexcept;

        [[nodiscard]] const NodeRecord* findNode(NodeId node) const noexcept;
        [[nodiscard]] const PinRecord* findPin(PinId pin) const noexcept;
        [[nodiscard]] const LinkRecord* findLink(PinId from, PinId to) const noexcept;
        [[nodiscard]] std::optional<LinkRecord> incoming(PinId input) const noexcept;
        [[nodiscard]] std::size_t linkCount(PinId pin) const noexcept;

        [[nodiscard]] std::span<const NodeRecord> nodes() const noexcept;
        [[nodiscard]] std::span<const PinRecord> pins() const noexcept;
        [[nodiscard]] std::span<const LinkRecord> links() const noexcept;

    private:
        [[nodiscard]] static GraphTopologyFailure failure(
            EGraphTopologyError code,
            NodeId node = {},
            PinId pin = {},
            PinId related_pin = {}
        ) noexcept;
        [[nodiscard]] lux::cxx::expected<void, GraphTopologyFailure> validateCombined(
            std::span<const NodeRecord> nodes,
            std::span<const PinRecord> pins,
            std::span<const LinkRecord> links
        ) const noexcept;
        void advanceNodeId(NodeId id) noexcept;
        void advancePinId(PinId id) noexcept;

        std::vector<NodeRecord> nodes_;
        std::vector<PinRecord> pins_;
        std::vector<LinkRecord> links_;
        std::uint64_t next_node_id_{1U};
        std::uint64_t next_pin_id_{1U};
    };
} // namespace lux::graph

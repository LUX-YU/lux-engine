#pragma once

#include <lux/engine/function/graph/GraphTypes.hpp>

#include <variant>

namespace lux::editor::node_graph
{
    struct ConnectIntent final { graph::PinId from; graph::PinId to; };
    struct DisconnectIntent final { graph::PinId from; graph::PinId to; };
    struct AddNodeIntent final { graph::NodeTypeId type; graph::GraphNodeLayout layout; };
    struct RemoveNodeIntent final { graph::NodeId node; };
    struct MoveNodeIntent final { graph::NodeId node; graph::GraphNodeLayout layout; };
    struct SelectNodeIntent final { graph::NodeId node; bool additive{}; };
    struct InvokeNodeActionIntent final { graph::NodeId node; std::uint64_t action{}; };

    using GraphIntent = std::variant<
        ConnectIntent,
        DisconnectIntent,
        AddNodeIntent,
        RemoveNodeIntent,
        MoveNodeIntent,
        SelectNodeIntent,
        InvokeNodeActionIntent
    >;
} // namespace lux::editor::node_graph

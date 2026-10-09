#pragma once
// =============================================================================
//  DataPin.hpp  —  Typed data pins and connections for the material graph
// -----------------------------------------------------------------------------
//  The material graph is pure dataflow: only data pins exist, none of FlowForge's
//  exec pins / tokens. Pin types use EValueType (a plain enum) and do not go
//  through the lux::meta reflection system.
//
//  Authoring owns this editable source model; cook lowers it to runtime data.
// =============================================================================

#include <cstdint>
#include <string>
#include <vector>

#include <lux/engine/material/graph/Types.hpp>
#include <lux/engine/function/graph/GraphTypes.hpp>

namespace lux::material
{
    inline constexpr uint32_t invalid_pin = ~0u;

    using lux::graph::NodeId;
    using lux::graph::PinId;

    /**
     * @brief The source of a connection: an output pin on some node.
     *        An input pin has at most one source (pure dataflow, fan-in = 1).
     */
    struct PinLink
    {
        NodeId node{};              ///< Source node
        uint32_t pin = invalid_pin; ///< Index of the output pin on the source node

        bool valid() const noexcept
        {
            return node.valid() && pin != invalid_pin;
        }
    };

    /**
     * @brief A typed data pin (an aggregate type, for easy initialization).
     *        Direction, ownership and links belong to GraphTopology.
     *        An unconnected input uses `constant`; outputs ignore it.
     */
    struct DataPin
    {
        std::string name;
        EValueType type = EValueType::FLOAT;
        float constant[4] = {0, 0, 0, 0}; ///< Default constant used when an input is unconnected
        PinId id;                         ///< Stable shared-topology identity
    };

} // namespace lux::material

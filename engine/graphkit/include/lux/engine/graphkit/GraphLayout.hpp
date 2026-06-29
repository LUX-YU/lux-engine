#pragma once
// =============================================================================
//  GraphLayout.hpp — GraphKit-owned fallback auto-layout.
//
//  Assigns grid positions to nodes whose nodePos() is unset and writes them
//  back through IGraphView::setNodePos (so they persist on the next save via
//  the domain's editor-metadata block). Mandatory day-1: leniently decoded
//  older assets (e.g. LMGR v1 blobs, which carry no positions) open with every
//  node unplaced. Centralizes the hand-rolled first-open grid that
//  MaterialGraphPanel carried (layout_done_).
// =============================================================================

#include <cstddef>
#include <cstdint>

#include "GraphTypes.hpp"
#include "IGraphView.hpp"

namespace lux::graphkit
{
    /// Lays out every UNPLACED node (nodePos() == nullopt) on a row-major grid
    /// starting at @p origin with @p cell spacing, @p columns per row. Placed
    /// nodes are untouched. Returns the number of nodes positioned.
    LUX_GRAPHKIT_PUBLIC std::size_t layoutUnplacedNodes(
        IGraphView&   view,
        GraphVec2     origin  = GraphVec2{ 60.0f, 60.0f },
        GraphVec2     cell    = GraphVec2{ 260.0f, 180.0f },
        std::uint32_t columns = 4
    );

} // namespace lux::graphkit

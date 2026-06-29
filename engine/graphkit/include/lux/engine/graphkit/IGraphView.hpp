#pragma once
// =============================================================================
//  IGraphView.hpp — the STATELESS read+command port over a borrowed domain
//  graph SSOT (rdesc::MaterialGraph, flowforge::FlowGraph, ...).
//
//  THE INVARIANT: graph data exists exactly once per domain. This port owns NO
//  data — it is a lens, so it physically cannot become a second copy of the
//  nodes/pins. Compile and serialize read the domain SSOT DIRECTLY, never
//  through this port; the port exists for the one genuinely shared concern:
//  EDITING.
//
//  Adapter contract (every domain adapter must honor):
//   (a) ID-STABILITY: a detached node is re-attachable under its ORIGINAL id
//       (the inverse-intent undo stack depends on it; links and editor refs
//       are keyed by node id). Domains provide id-preserving reinsert —
//       flowforge FlowGraph::insertNodeAt, material addNodeWithId (P4).
//   (b) NO IMPLICIT SEVERANCE: connect() must NOT silently break an existing
//       link. The editor reads fan caps from GraphPinType and performs the
//       explicit disconnect(old)+connect(new) pair itself (two recorded
//       inverse intents).
//   (c) PIN-INDEX STABILITY: pin indices stay valid until reconstructNode()
//       (which rebuilds the node's dynamic pins; the editor then re-reads).
//  Snapshots (GraphPinView/GraphLinkView and the title passed to forEachNode)
//  borrow domain memory and are valid only during the enclosing call.
// =============================================================================

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include "GraphTypes.hpp"

namespace lux::graphkit
{
    /// Opaque OWNING handle to a detached node. The domain adapter knows what
    /// is inside (e.g. a moved-out flowforge NodeStorage, or a cloned rdesc
    /// node); shared_ptr<void> type-erases ownership of move-only payloads.
    /// In-memory capture is deliberately used instead of codec bytes: it is
    /// perfectly faithful (constants, reflection pointers) and needs no
    /// domain codec at editing time.
    using NodeCapture = std::shared_ptr<void>;

    class IGraphView
    {
    public:
        virtual ~IGraphView() = default;

        // ---- read core (per-frame; never store the views) -------------------
        /// Visits every node with its display TITLE (both shipped panels draw
        /// node headers from domain names; also feeds future search/navigate).
        virtual void forEachNode(
            const std::function<void(GraphNodeRef, std::string_view title)>& fn) const = 0;

        virtual std::uint32_t pinCount(GraphNodeRef node, EPinSide side) const = 0;

        /// Snapshot of one pin: display label + opaque draw type + linked-ness.
        virtual GraphPinView pin(GraphNodeRef node, EPinSide side, std::uint32_t index) const = 0;

        /// Visits every link (from = output pin, to = input pin).
        virtual void forEachLink(const std::function<void(GraphLinkView)>& fn) const = 0;

        /// Canvas position, if the domain has one stored for this node.
        /// nullopt => unplaced (the editor runs the fallback grid layout and
        /// writes the result back through setNodePos).
        virtual std::optional<GraphVec2> nodePos(GraphNodeRef node) const = 0;
        virtual void setNodePos(GraphNodeRef node, GraphVec2 pos) = 0;

        // ---- commands (delegate to the domain SSOT's own mutators) ----------
        /// Creates a node from a palette template id (the factory lives
        /// domain-side: NodeRegistry creator / EMatNodeKind switch). The new
        /// node allocates its own pins in its constructor.
        virtual GraphNodeRef addNode(std::string_view template_id) = 0;

        /// Detaches a node, returning an owning capture for undo. The node
        /// must have no incident links — the editor disconnects them first
        /// (each as a recorded inverse intent).
        virtual NodeCapture detachNode(GraphNodeRef node) = 0;

        /// Re-attaches a previously detached node under its ORIGINAL id
        /// (contract (a)). Returns false if the id is occupied.
        virtual bool attachNode(GraphNodeRef original, NodeCapture capture) = 0;

        /// Connects an output pin to an input pin. Must not implicitly sever
        /// (contract (b)); returns false if the domain rejects the link.
        virtual bool connect(GraphPinRef from, GraphPinRef to) = 0;

        /// Removes the specific link (both endpoints identify it — flowforge
        /// exec-in pins fan-in from multiple sources).
        virtual bool disconnect(GraphPinRef from, GraphPinRef to) = 0;

        /// THE one arity-rebuild entry (UE AllocateDefaultPins/ReconstructNode
        /// analogue): asks the domain node to rebuild its dynamic pins
        /// (material setType, flowforge rebind/addExecOutPin). The editor then
        /// purges this node's pin-id mappings, re-reads pins, and re-validates
        /// previously recorded links via the schema.
        virtual void reconstructNode(GraphNodeRef node) = 0;
    };

} // namespace lux::graphkit

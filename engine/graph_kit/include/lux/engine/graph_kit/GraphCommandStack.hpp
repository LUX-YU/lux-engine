#pragma once
// =============================================================================
//  GraphCommandStack.hpp — inverse-INTENT undo/redo over an IGraphView.
//
//  NOT a model snapshot: a snapshot cannot safely restore domain graph's
//  non-copyable, recycling-id pointer graph. Every recorded primitive calls
//  the domain's own typed mutators through the port, and node removal holds an
//  in-memory NodeCapture (perfectly faithful — constants, reflection pointers
//  — and needs no domain codec at editing time).
//
//  THE ID-STABILITY CONTRACT (correctness keystone): undo must restore removed
//  nodes under their ORIGINAL ids — links and recorded intents reference node
//  ids. IGraphView::attachNode is required to honor this (domain graph:
//  FlowGraph::insertNodeAt reconciling the recycling allocator; material:
//  addNodeWithId with a next_id high-water bump). LIFO replay guarantees the
//  original id is free again by the time its restore runs.
//
//  SCOPE (v2 day-1 decisions, recorded):
//   * Node-body PAYLOAD edits (constant values, texture bindings, colors via
//     IGraphSchema::drawNodeBody) are OUT of undo scope — today's panels have
//     zero undo, so parity does not demand it. Future additive command kind.
//   * reconstructNode (arity rebuild) is not undoable day-1 for the same
//     reason.
//   * Schema hooks (onPinConnected — wildcard resolution) fire on INTERACTIVE
//     edits in GraphEditor, not during undo/redo replay; no domain emits
//     wildcards yet.
//
//  CAP-AWARE REPLACE-ON-RECONNECT: doConnect reads both endpoints' fan caps
//  (GraphPinType::fan_cap); a cap-1 endpoint that is already linked gets its
//  old link explicitly disconnected FIRST, recorded as its own inverse intent
//  in the same transaction — adapters must never implicitly sever (port
//  contract (b)). Callers validate via IGraphSchema::canConnect BEFORE
//  doConnect; the stack does not consult the schema.
// =============================================================================

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "GraphTypes.hpp"
#include "IGraphView.hpp"

namespace lux::graphkit
{
    class LUX_GRAPHKIT_PUBLIC GraphCommandStack
    {
    public:
        explicit GraphCommandStack(IGraphView* view = nullptr);

        /// Re-targets the stack at another graph. FULL RESET (the rebind
        /// contract): both stacks are cleared — recorded intents reference the
        /// previous graph's ids.
        void bind(IGraphView* view);

        void clear();

        // ---- transactions ----------------------------------------------------
        /// Groups subsequent do* calls into ONE undo step (e.g. delete-node =
        /// N disconnects + 1 detach). Primitives called outside a transaction
        /// auto-wrap themselves.
        void beginTransaction(std::string label);
        void commitTransaction();
        bool inTransaction() const noexcept { return in_transaction_; }

        // ---- recorded primitives (execute through the view + record inverse) -
        /// Adds a node from a palette template (optionally placing it).
        /// Returns the new node (invalid on domain failure).
        GraphNodeRef doAddNode(std::string_view template_id,
                               std::optional<GraphVec2> pos = std::nullopt);

        /// Removes a node UNDOABLY: disconnects every incident link (recorded),
        /// then detaches the node, holding the capture for undo. One undo step.
        bool doRemoveNode(GraphNodeRef node);

        /// Connects from(output) -> to(input) with cap-aware replace (see
        /// header). Returns false if the domain rejects the link.
        bool doConnect(GraphPinRef from, GraphPinRef to);

        bool doDisconnect(GraphPinRef from, GraphPinRef to);

        void doMoveNode(GraphNodeRef node, GraphVec2 new_pos);

        /// Drag-gesture flavor: the view may already hold the FINAL position
        /// (live write-back while dragging); the caller supplies the position
        /// at drag START so undo returns there.
        void doMoveNode(GraphNodeRef node, GraphVec2 old_pos, GraphVec2 new_pos);

        // ---- undo / redo ------------------------------------------------------
        bool undo();
        bool redo();
        bool canUndo() const noexcept { return !undo_stack_.empty(); }
        bool canRedo() const noexcept { return !redo_stack_.empty(); }
        std::size_t undoDepth() const noexcept { return undo_stack_.size(); }

        /// Monotonic counter of STRUCTURAL graph changes: bumps when a
        /// committed transaction (or an undo/redo replay of one) contains any
        /// op besides MOVE_NODE. Hosts poll it for bake-on-edit ("graph shape
        /// changed since I last compiled?") without a callback seam — node
        /// moves never trigger a rebake.
        std::uint64_t structureRevision() const noexcept { return structure_revision_; }

    private:
        struct Op
        {
            enum class EKind : std::uint8_t
            {
                CONNECT,
                DISCONNECT,
                ADD_NODE,
                REMOVE_NODE,
                MOVE_NODE
            };

            EKind        kind{};
            GraphPinRef  from;       ///< CONNECT / DISCONNECT
            GraphPinRef  to;         ///< CONNECT / DISCONNECT
            GraphNodeRef node;       ///< ADD_NODE / REMOVE_NODE / MOVE_NODE
            NodeCapture  capture;    ///< REMOVE: held after execute; ADD: held after undo
            GraphVec2    old_pos;    ///< MOVE_NODE
            GraphVec2    new_pos;    ///< MOVE_NODE
        };

        struct Transaction
        {
            std::string     label;
            std::vector<Op> ops;
        };

        void record(Op op);
        bool applyUndo(Op& op);
        bool applyRedo(Op& op);
        /// True when @p t holds any non-MOVE op (see structureRevision).
        static bool isStructural(const Transaction& t) noexcept;

        IGraphView*              view_ = nullptr;
        std::vector<Transaction> undo_stack_;
        std::vector<Transaction> redo_stack_;
        Transaction              pending_;
        bool                     in_transaction_ = false;
        std::uint64_t            structure_revision_ = 0;
    };

} // namespace lux::graphkit

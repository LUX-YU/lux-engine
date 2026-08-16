#include <lux/engine/editor/framework/graphkit/GraphCommandStack.hpp>

#include <utility>

namespace lux::graphkit
{
    GraphCommandStack::GraphCommandStack(IGraphView* view)
        : view_(view)
    {
    }

    void GraphCommandStack::bind(IGraphView* view)
    {
        view_ = view;
        clear();
    }

    void GraphCommandStack::clear()
    {
        undo_stack_.clear();
        redo_stack_.clear();
        pending_ = Transaction{};
        in_transaction_ = false;
    }

    // ---- transactions ---------------------------------------------------------

    void GraphCommandStack::beginTransaction(std::string label)
    {
        // Nested begin is collapsed into the outer transaction (the outer
        // commit closes it) — keeps composed operations one undo step.
        if (in_transaction_)
        {
            return;
        }
        pending_ = Transaction{ std::move(label), {} };
        in_transaction_ = true;
    }

    void GraphCommandStack::commitTransaction()
    {
        if (!in_transaction_)
        {
            return;
        }
        in_transaction_ = false;
        if (pending_.ops.empty())
        {
            return;
        }
        if (isStructural(pending_))
        {
            ++structure_revision_;
        }
        undo_stack_.push_back(std::move(pending_));
        pending_ = Transaction{};
        // Any new committed edit invalidates the redo branch.
        redo_stack_.clear();
    }

    bool GraphCommandStack::isStructural(const Transaction& t) noexcept
    {
        for (const auto& op : t.ops)
        {
            if (op.kind != Op::EKind::MOVE_NODE)
            {
                return true;
            }
        }
        return false;
    }

    void GraphCommandStack::record(Op op)
    {
        pending_.ops.push_back(std::move(op));
    }

    // ---- recorded primitives ----------------------------------------------------

    GraphNodeRef GraphCommandStack::doAddNode(std::string_view template_id,
                                              std::optional<GraphVec2> pos)
    {
        if (!view_)
        {
            return {};
        }
        const bool auto_tx = !in_transaction_;
        if (auto_tx)
        {
            beginTransaction("add node");
        }

        const GraphNodeRef node = view_->addNode(template_id);
        if (node.valid())
        {
            if (pos)
            {
                view_->setNodePos(node, *pos);
            }
            Op op;
            op.kind = Op::EKind::ADD_NODE;
            op.node = node;
            record(std::move(op));
        }

        if (auto_tx)
        {
            commitTransaction();
        }
        return node;
    }

    bool GraphCommandStack::doRemoveNode(GraphNodeRef node)
    {
        if (!view_ || !node.valid())
        {
            return false;
        }
        const bool auto_tx = !in_transaction_;
        if (auto_tx)
        {
            beginTransaction("remove node");
        }

        // Disconnect every incident link first — each one a recorded inverse
        // intent, so the SINGLE undo step restores the node AND its links.
        std::vector<GraphLinkView> incident;
        view_->forEachLink(
            [&](GraphLinkView link)
            {
                if (link.from.node == node || link.to.node == node)
                {
                    incident.push_back(link);
                }
            }
        );
        for (const GraphLinkView& link : incident)
        {
            doDisconnect(link.from, link.to);
        }

        Op op;
        op.kind    = Op::EKind::REMOVE_NODE;
        op.node    = node;
        op.capture = view_->detachNode(node);
        const bool ok = (op.capture != nullptr);
        if (ok)
        {
            record(std::move(op));
        }

        if (auto_tx)
        {
            commitTransaction();
        }
        return ok;
    }

    bool GraphCommandStack::doConnect(GraphPinRef from, GraphPinRef to)
    {
        if (!view_ || !from.valid() || !to.valid())
        {
            return false;
        }
        const bool auto_tx = !in_transaction_;
        if (auto_tx)
        {
            beginTransaction("connect");
        }

        // Cap-aware replace-on-reconnect: a cap-1 endpoint that is already
        // linked yields its old link via an EXPLICIT recorded disconnect.
        // (Only cap 1 triggers replacement; other caps either never fill
        // (kFanUnlimited) or are left to the domain connect to reject.)
        // Collect first, mutate after — never disconnect while the view is
        // iterating its own link container.
        std::vector<GraphLinkView> yielding;
        const GraphPinView to_view   = view_->pin(to.node, to.side, to.pin);
        const GraphPinView from_view = view_->pin(from.node, from.side, from.pin);
        if ((to_view.type.fan_cap == 1 && to_view.has_link) ||
            (from_view.type.fan_cap == 1 && from_view.has_link))
        {
            view_->forEachLink(
                [&](GraphLinkView link)
                {
                    const bool to_yields =
                        to_view.type.fan_cap == 1 && to_view.has_link && link.to == to;
                    const bool from_yields =
                        from_view.type.fan_cap == 1 && from_view.has_link && link.from == from;
                    if (to_yields || from_yields)
                    {
                        yielding.push_back(link);
                    }
                }
            );
        }
        for (const GraphLinkView& link : yielding)
        {
            doDisconnect(link.from, link.to);
        }

        const bool ok = view_->connect(from, to);
        if (ok)
        {
            Op op;
            op.kind = Op::EKind::CONNECT;
            op.from = from;
            op.to   = to;
            record(std::move(op));
        }

        if (auto_tx)
        {
            commitTransaction();
        }
        return ok;
    }

    bool GraphCommandStack::doDisconnect(GraphPinRef from, GraphPinRef to)
    {
        if (!view_)
        {
            return false;
        }
        const bool auto_tx = !in_transaction_;
        if (auto_tx)
        {
            beginTransaction("disconnect");
        }

        const bool ok = view_->disconnect(from, to);
        if (ok)
        {
            Op op;
            op.kind = Op::EKind::DISCONNECT;
            op.from = from;
            op.to   = to;
            record(std::move(op));
        }

        if (auto_tx)
        {
            commitTransaction();
        }
        return ok;
    }

    void GraphCommandStack::doMoveNode(GraphNodeRef node, GraphVec2 new_pos)
    {
        if (!view_)
        {
            return;
        }
        doMoveNode(node, view_->nodePos(node).value_or(GraphVec2{}), new_pos);
    }

    void GraphCommandStack::doMoveNode(GraphNodeRef node, GraphVec2 old_pos, GraphVec2 new_pos)
    {
        if (!view_ || !node.valid())
        {
            return;
        }
        const bool auto_tx = !in_transaction_;
        if (auto_tx)
        {
            beginTransaction("move node");
        }

        Op op;
        op.kind    = Op::EKind::MOVE_NODE;
        op.node    = node;
        op.old_pos = old_pos;
        op.new_pos = new_pos;
        view_->setNodePos(node, new_pos);
        record(std::move(op));

        if (auto_tx)
        {
            commitTransaction();
        }
    }

    // ---- undo / redo ------------------------------------------------------------

    bool GraphCommandStack::applyUndo(Op& op)
    {
        switch (op.kind)
        {
        case Op::EKind::CONNECT:
            return view_->disconnect(op.from, op.to);
        case Op::EKind::DISCONNECT:
            return view_->connect(op.from, op.to);
        case Op::EKind::ADD_NODE:
            // Undo of add = detach; hold the capture so redo can re-attach the
            // SAME node under the SAME id.
            op.capture = view_->detachNode(op.node);
            return op.capture != nullptr;
        case Op::EKind::REMOVE_NODE:
            // The id-stability contract: restore under the ORIGINAL id.
            if (!view_->attachNode(op.node, op.capture))
            {
                return false;
            }
            op.capture = nullptr;
            return true;
        case Op::EKind::MOVE_NODE:
            view_->setNodePos(op.node, op.old_pos);
            return true;
        }
        return false;
    }

    bool GraphCommandStack::applyRedo(Op& op)
    {
        switch (op.kind)
        {
        case Op::EKind::CONNECT:
            return view_->connect(op.from, op.to);
        case Op::EKind::DISCONNECT:
            return view_->disconnect(op.from, op.to);
        case Op::EKind::ADD_NODE:
            if (!view_->attachNode(op.node, op.capture))
            {
                return false;
            }
            op.capture = nullptr;
            return true;
        case Op::EKind::REMOVE_NODE:
            op.capture = view_->detachNode(op.node);
            return op.capture != nullptr;
        case Op::EKind::MOVE_NODE:
            view_->setNodePos(op.node, op.new_pos);
            return true;
        }
        return false;
    }

    bool GraphCommandStack::undo()
    {
        if (!view_ || undo_stack_.empty() || in_transaction_)
        {
            return false;
        }
        Transaction tx = std::move(undo_stack_.back());
        undo_stack_.pop_back();

        // Replay inverses in REVERSE order (LIFO guarantees ids are free again
        // by the time their restore runs).
        bool ok = true;
        for (auto it = tx.ops.rbegin(); it != tx.ops.rend(); ++it)
        {
            ok = applyUndo(*it) && ok;
        }
        if (isStructural(tx))
        {
            ++structure_revision_;
        }
        redo_stack_.push_back(std::move(tx));
        return ok;
    }

    bool GraphCommandStack::redo()
    {
        if (!view_ || redo_stack_.empty() || in_transaction_)
        {
            return false;
        }
        Transaction tx = std::move(redo_stack_.back());
        redo_stack_.pop_back();

        bool ok = true;
        for (auto& op : tx.ops)
        {
            ok = applyRedo(op) && ok;
        }
        if (isStructural(tx))
        {
            ++structure_revision_;
        }
        undo_stack_.push_back(std::move(tx));
        return ok;
    }

} // namespace lux::graphkit

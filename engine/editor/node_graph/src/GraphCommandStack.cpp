#include <lux/engine/editor/node_graph/GraphCommandStack.hpp>

#include <new>
#include <type_traits>
#include <utility>

namespace lux::editor::node_graph
{
    GraphCommandStack::GraphCommandStack(IGraphView* view)
        : view_(view)
    {
    }

    void GraphCommandStack::bind(IGraphView* view)
    {
        if (in_transaction_)
        {
            failTransaction();
        }
        view_ = view;
        undo_stack_.clear();
        redo_stack_.clear();
        pending_ = Transaction{};
        in_transaction_ = false;
        poisoned_ = false;
    }

    void GraphCommandStack::clear()
    {
        if (in_transaction_)
        {
            failTransaction();
            if (poisoned_)
            {
                return;
            }
        }
        undo_stack_.clear();
        redo_stack_.clear();
        pending_ = Transaction{};
        in_transaction_ = false;
    }

    // ---- transactions ---------------------------------------------------------

    bool GraphCommandStack::beginTransaction(std::string label)
    {
        if (!view_ || in_transaction_ || poisoned_)
        {
            return false;
        }
        try
        {
            undo_stack_.reserve(undo_stack_.size() + 1U);
            pending_ = Transaction{std::move(label), {}};
        }
        catch (const std::bad_alloc&)
        {
            pending_ = Transaction{};
            return false;
        }
        in_transaction_ = true;
        return true;
    }

    bool GraphCommandStack::commitTransaction()
    {
        if (!in_transaction_ || poisoned_)
        {
            return false;
        }
        in_transaction_ = false;
        if (pending_.ops.empty())
        {
            pending_ = Transaction{};
            return true;
        }
        const bool structural = isStructural(pending_);
        undo_stack_.push_back(std::move(pending_));
        pending_ = Transaction{};
        redo_stack_.clear();
        if (structural)
        {
            ++structure_revision_;
        }
        return true;
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
        static_assert(std::is_nothrow_move_constructible_v<Op>);
        pending_.ops.push_back(std::move(op));
    }

    bool GraphCommandStack::prepareTransaction(std::string label, bool& automatic)
    {
        automatic = !in_transaction_;
        return !automatic || beginTransaction(std::move(label));
    }

    bool GraphCommandStack::reservePending(std::size_t additional)
    {
        try
        {
            pending_.ops.reserve(pending_.ops.size() + additional);
            return true;
        }
        catch (const std::bad_alloc&)
        {
            return failTransaction();
        }
    }

    bool GraphCommandStack::failTransaction()
    {
        if (!in_transaction_)
        {
            return false;
        }
        bool restored = true;
        for (auto operation = pending_.ops.rbegin(); operation != pending_.ops.rend(); ++operation)
        {
            if (!applyUndo(*operation))
            {
                restored = false;
                break;
            }
        }
        pending_ = Transaction{};
        in_transaction_ = false;
        if (!restored)
        {
            poisoned_ = true;
        }
        return false;
    }

    // ---- recorded primitives ----------------------------------------------------

    GraphNodeRef GraphCommandStack::doAddNode(std::string_view template_id,
                                              std::optional<GraphVec2> pos)
    {
        bool automatic = false;
        if (!prepareTransaction("add node", automatic) || !reservePending(1U))
        {
            return {};
        }

        const GraphNodeRef node = view_->addNode(template_id);
        if (!node.valid())
        {
            failTransaction();
            return {};
        }
        Op op;
        op.kind = Op::EKind::ADD_NODE;
        op.node = node;
        record(std::move(op));
        if (pos)
        {
            view_->setNodePos(node, *pos);
        }
        const bool committed = !automatic || commitTransaction();
        return committed ? node : GraphNodeRef{};
    }

    bool GraphCommandStack::doRemoveNode(GraphNodeRef node)
    {
        if (!node.valid())
        {
            if (in_transaction_)
            {
                failTransaction();
            }
            return false;
        }
        bool automatic = false;
        if (!prepareTransaction("remove node", automatic))
        {
            return false;
        }

        // Disconnect every incident link first — each one a recorded inverse
        // intent, so the SINGLE undo step restores the node AND its links.
        std::vector<GraphLinkView> incident;
        try
        {
            view_->forEachLink(
                [&](GraphLinkView link)
                {
                    if (link.from.node == node || link.to.node == node)
                    {
                        incident.push_back(link);
                    }
                }
            );
        }
        catch (const std::bad_alloc&)
        {
            return failTransaction();
        }
        if (!reservePending(incident.size() + 1U))
        {
            return false;
        }
        for (const GraphLinkView& link : incident)
        {
            if (!view_->disconnect(link.from, link.to))
            {
                return failTransaction();
            }
            Op disconnect;
            disconnect.kind = Op::EKind::DISCONNECT;
            disconnect.from = link.from;
            disconnect.to = link.to;
            record(std::move(disconnect));
        }

        Op op;
        op.kind    = Op::EKind::REMOVE_NODE;
        op.node    = node;
        op.capture = view_->detachNode(node);
        const bool ok = (op.capture != nullptr);
        if (!ok)
        {
            return failTransaction();
        }
        record(std::move(op));

        return !automatic || commitTransaction();
    }

    bool GraphCommandStack::doConnect(GraphPinRef from, GraphPinRef to)
    {
        if (!from.valid() || !to.valid())
        {
            if (in_transaction_)
            {
                failTransaction();
            }
            return false;
        }
        bool automatic = false;
        if (!prepareTransaction("connect", automatic))
        {
            return false;
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
        try
        {
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
        }
        catch (const std::bad_alloc&)
        {
            return failTransaction();
        }
        if (!reservePending(yielding.size() + 1U))
        {
            return false;
        }
        for (const GraphLinkView& link : yielding)
        {
            if (!view_->disconnect(link.from, link.to))
            {
                return failTransaction();
            }
            Op disconnect;
            disconnect.kind = Op::EKind::DISCONNECT;
            disconnect.from = link.from;
            disconnect.to = link.to;
            record(std::move(disconnect));
        }

        const bool ok = view_->connect(from, to);
        if (!ok)
        {
            return failTransaction();
        }
        Op op;
        op.kind = Op::EKind::CONNECT;
        op.from = from;
        op.to   = to;
        record(std::move(op));

        return !automatic || commitTransaction();
    }

    bool GraphCommandStack::doDisconnect(GraphPinRef from, GraphPinRef to)
    {
        bool automatic = false;
        if (!prepareTransaction("disconnect", automatic) || !reservePending(1U))
        {
            return false;
        }

        const bool ok = view_->disconnect(from, to);
        if (!ok)
        {
            return failTransaction();
        }
        Op op;
        op.kind = Op::EKind::DISCONNECT;
        op.from = from;
        op.to   = to;
        record(std::move(op));

        return !automatic || commitTransaction();
    }

    bool GraphCommandStack::doMoveNode(GraphNodeRef node, GraphVec2 new_pos)
    {
        if (!view_)
        {
            return false;
        }
        return doMoveNode(node, view_->nodePos(node).value_or(GraphVec2{}), new_pos);
    }

    bool GraphCommandStack::doMoveNode(GraphNodeRef node, GraphVec2 old_pos, GraphVec2 new_pos)
    {
        if (!node.valid())
        {
            if (in_transaction_)
            {
                failTransaction();
            }
            return false;
        }
        bool automatic = false;
        if (!prepareTransaction("move node", automatic))
        {
            return false;
        }
        if (!reservePending(1U))
        {
            view_->setNodePos(node, old_pos);
            return false;
        }

        Op op;
        op.kind    = Op::EKind::MOVE_NODE;
        op.node    = node;
        op.old_pos = old_pos;
        op.new_pos = new_pos;
        record(std::move(op));
        view_->setNodePos(node, new_pos);

        return !automatic || commitTransaction();
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

    bool GraphCommandStack::rollbackUndoPrefix(Transaction& transaction, std::size_t first_applied)
    {
        for (std::size_t index = first_applied; index < transaction.ops.size(); ++index)
        {
            if (!applyRedo(transaction.ops[index]))
            {
                poisoned_ = true;
                return false;
            }
        }
        return true;
    }

    bool GraphCommandStack::rollbackRedoPrefix(Transaction& transaction, std::size_t applied_count)
    {
        for (std::size_t index = applied_count; index > 0U; --index)
        {
            if (!applyUndo(transaction.ops[index - 1U]))
            {
                poisoned_ = true;
                return false;
            }
        }
        return true;
    }

    bool GraphCommandStack::undo()
    {
        if (!view_ || undo_stack_.empty() || in_transaction_ || poisoned_)
        {
            return false;
        }
        try
        {
            redo_stack_.reserve(redo_stack_.size() + 1U);
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }

        auto& transaction = undo_stack_.back();
        for (std::size_t index = transaction.ops.size(); index > 0U; --index)
        {
            if (!applyUndo(transaction.ops[index - 1U]))
            {
                rollbackUndoPrefix(transaction, index);
                return false;
            }
        }
        const bool structural = isStructural(transaction);
        redo_stack_.push_back(std::move(transaction));
        undo_stack_.pop_back();
        if (structural)
        {
            ++structure_revision_;
        }
        return true;
    }

    bool GraphCommandStack::redo()
    {
        if (!view_ || redo_stack_.empty() || in_transaction_ || poisoned_)
        {
            return false;
        }
        try
        {
            undo_stack_.reserve(undo_stack_.size() + 1U);
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }

        auto& transaction = redo_stack_.back();
        for (std::size_t index = 0U; index < transaction.ops.size(); ++index)
        {
            if (!applyRedo(transaction.ops[index]))
            {
                rollbackRedoPrefix(transaction, index);
                return false;
            }
        }
        const bool structural = isStructural(transaction);
        undo_stack_.push_back(std::move(transaction));
        redo_stack_.pop_back();
        if (structural)
        {
            ++structure_revision_;
        }
        return true;
    }

} // namespace lux::editor::node_graph

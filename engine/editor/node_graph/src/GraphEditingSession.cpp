#include <lux/engine/editor/node_graph/GraphEditingSession.hpp>

#include <algorithm>
#include <new>
#include <utility>

namespace lux::editor::node_graph
{
    GraphEditingSession::GraphEditingSession(IGraphDocument& document, const IGraphRules& rules) noexcept
        : document_(&document), rules_(&rules)
    {
    }

    bool GraphEditingSession::apply(const GraphIntent& intent)
    {
        return std::visit([this](const auto& value) -> bool {
            using Type = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::same_as<Type, ConnectIntent>)
                return applyConnect(value.from, value.to);
            else if constexpr (std::same_as<Type, DisconnectIntent>)
                return applyDisconnect(value.from, value.to);
            else if constexpr (std::same_as<Type, AddNodeIntent>)
                return applyAdd(value);
            else if constexpr (std::same_as<Type, RemoveNodeIntent>)
                return applyRemove(value.node);
            else if constexpr (std::same_as<Type, MoveNodeIntent>)
                return applyMove(value);
            else if constexpr (std::same_as<Type, SelectNodeIntent>)
            {
                if (value.node.valid() && document_->topology().findNode(value.node) == nullptr)
                    return false;
                selected_ = value.node;
                return true;
            }
            else
            {
                return applyAction(value);
            }
        }, intent);
    }

    bool GraphEditingSession::beginTransaction(std::string label)
    {
        if (poisoned_ || in_transaction_ || topology_locked_)
            return false;
        try
        {
            pending_ = Transaction{std::move(label), {}, false};
            undo_.reserve(undo_.size() + 1U);
            in_transaction_ = true;
            return true;
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
    }

    bool GraphEditingSession::commitTransaction()
    {
        if (!in_transaction_ || poisoned_)
            return false;
        in_transaction_ = false;
        if (pending_.operations.empty())
        {
            pending_ = {};
            return true;
        }
        try
        {
            undo_.push_back(std::move(pending_));
            redo_.clear();
            if (undo_.back().structural)
                ++revision_;
            pending_ = {};
            return true;
        }
        catch (const std::bad_alloc&)
        {
            return fail();
        }
    }

    bool GraphEditingSession::prepare(std::string label, std::size_t operation_capacity, bool& automatic)
    {
        if (poisoned_ || topology_locked_)
            return false;
        automatic = !in_transaction_;
        if (automatic && !beginTransaction(std::move(label)))
            return false;
        try
        {
            pending_.operations.reserve(pending_.operations.size() + operation_capacity);
            if (automatic)
                undo_.reserve(undo_.size() + 1U);
            return true;
        }
        catch (const std::bad_alloc&)
        {
            if (automatic)
            {
                pending_ = {};
                in_transaction_ = false;
            }
            return false;
        }
    }

    bool GraphEditingSession::finish(bool automatic)
    {
        return !automatic || commitTransaction();
    }

    void GraphEditingSession::record(Operation operation)
    {
        pending_.structural |= operation.kind != EOperation::MOVE;
        pending_.operations.push_back(std::move(operation));
    }

    bool GraphEditingSession::fail()
    {
        bool restored = true;
        for (auto iterator = pending_.operations.rbegin(); iterator != pending_.operations.rend(); ++iterator)
            restored &= applyUndo(*iterator);
        pending_ = {};
        in_transaction_ = false;
        if (!restored)
            poisoned_ = true;
        return false;
    }

    bool GraphEditingSession::applyConnect(graph::PinId first, graph::PinId second)
    {
        const auto* first_pin = document_->topology().findPin(first);
        const auto* second_pin = document_->topology().findPin(second);
        if (first_pin == nullptr || second_pin == nullptr)
            return false;
        const auto from = first_pin->direction == graph::EPinDirection::OUTPUT ? first : second;
        const auto to = first_pin->direction == graph::EPinDirection::INPUT ? first : second;
        if (document_->topology().findLink(from, to) != nullptr || !rules_->canConnect(*document_, from, to))
            return false;

        std::vector<graph::LinkRecord> displaced;
        try
        {
            const auto* from_record = document_->topology().findPin(from);
            const auto* to_record = document_->topology().findPin(to);
            const bool from_full = from_record->fan_cap != graph::kUnlimitedFan &&
                document_->topology().linkCount(from) >= from_record->fan_cap;
            const bool to_full = to_record->fan_cap != graph::kUnlimitedFan &&
                document_->topology().linkCount(to) >= to_record->fan_cap;
            for (const auto& link : document_->topology().links())
            {
                const bool displace_from = from_full && (link.from == from || link.to == from);
                const bool displace_to = to_full && (link.from == to || link.to == to);
                if (displace_from || displace_to)
                    displaced.push_back(link);
            }
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }

        bool automatic{};
        if (!prepare("connect", displaced.size() + 1U, automatic))
            return false;
        for (const auto& link : displaced)
        {
            if (!document_->topology().disconnect(link.from, link.to))
                return fail();
            record(Operation{EOperation::DISCONNECT, {}, link.from, link.to});
        }
        if (!document_->topology().connect(from, to))
            return fail();
        record(Operation{EOperation::CONNECT, {}, from, to});
        return finish(automatic);
    }

    bool GraphEditingSession::applyDisconnect(graph::PinId from, graph::PinId to)
    {
        bool automatic{};
        if (!prepare("disconnect", 1U, automatic))
            return false;
        if (!document_->topology().disconnect(from, to))
            return fail();
        record(Operation{EOperation::DISCONNECT, {}, from, to});
        return finish(automatic);
    }

    bool GraphEditingSession::applyAdd(const AddNodeIntent& intent)
    {
        bool automatic{};
        if (!prepare("add node", 1U, automatic))
            return false;
        const auto node = document_->addNode(intent.type);
        if (!node.valid())
            return fail();
        if (!document_->layout().set(node, intent.layout))
        {
            static_cast<void>(document_->detachNode(node));
            return fail();
        }
        record(Operation{EOperation::ADD, node});
        selected_ = node;
        return finish(automatic);
    }

    bool GraphEditingSession::applyRemove(graph::NodeId node)
    {
        std::vector<graph::LinkRecord> links;
        try
        {
            for (const auto& link : document_->topology().links())
            {
                const auto* from = document_->topology().findPin(link.from);
                const auto* to = document_->topology().findPin(link.to);
                if ((from != nullptr && from->owner == node) || (to != nullptr && to->owner == node))
                    links.push_back(link);
            }
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
        bool automatic{};
        if (!prepare("remove node", links.size() + 1U, automatic))
            return false;
        for (const auto& link : links)
        {
            if (!document_->topology().disconnect(link.from, link.to))
                return fail();
            record(Operation{EOperation::DISCONNECT, {}, link.from, link.to});
        }
        auto capture = document_->detachNode(node);
        if (!capture)
            return fail();
        record(Operation{EOperation::REMOVE, node, {}, {}, {}, {}, std::move(capture)});
        if (selected_ == node)
            selected_ = {};
        return finish(automatic);
    }

    bool GraphEditingSession::applyMove(const MoveNodeIntent& intent)
    {
        if (document_->topology().findNode(intent.node) == nullptr)
            return false;
        const auto* current = document_->layout().find(intent.node);
        const auto before = current == nullptr ? graph::GraphNodeLayout{} : *current;
        if (before == intent.layout)
            return true;
        bool automatic{};
        if (!prepare("move node", 1U, automatic))
            return false;
        if (!document_->layout().set(intent.node, intent.layout))
            return fail();
        record(Operation{EOperation::MOVE, intent.node, {}, {}, before, intent.layout});
        return finish(automatic);
    }

    bool GraphEditingSession::applyAction(const InvokeNodeActionIntent& intent)
    {
        const bool is_invalid_node = !intent.node.valid() ||
            document_->topology().findNode(intent.node) == nullptr;
        if (is_invalid_node)
            return false;
        bool automatic{};
        if (!prepare("node action", 1U, automatic))
            return false;
        auto journal = document_->invokeNodeAction(intent.node, intent.action);
        if (!journal)
            return fail();
        Operation operation;
        operation.kind = EOperation::ACTION;
        operation.node = intent.node;
        operation.action = std::move(*journal);
        record(std::move(operation));
        return finish(automatic);
    }

    bool GraphEditingSession::applyUndo(Operation& operation)
    {
        switch (operation.kind)
        {
        case EOperation::ADD:
            operation.capture = document_->detachNode(operation.node);
            return static_cast<bool>(operation.capture);
        case EOperation::REMOVE:
            if (!operation.capture || !document_->attachNode(operation.node, operation.capture))
                return false;
            operation.capture.reset();
            return true;
        case EOperation::CONNECT:
            return static_cast<bool>(document_->topology().disconnect(operation.from, operation.to));
        case EOperation::DISCONNECT:
            return static_cast<bool>(document_->topology().connect(operation.from, operation.to));
        case EOperation::MOVE:
            return static_cast<bool>(document_->layout().set(operation.node, operation.before));
        case EOperation::ACTION:
            return document_->restoreNodeAction(operation.node, operation.action.before);
        }
        return false;
    }

    bool GraphEditingSession::applyRedo(Operation& operation)
    {
        switch (operation.kind)
        {
        case EOperation::ADD:
            if (!operation.capture || !document_->attachNode(operation.node, operation.capture))
                return false;
            operation.capture.reset();
            return true;
        case EOperation::REMOVE:
            operation.capture = document_->detachNode(operation.node);
            return static_cast<bool>(operation.capture);
        case EOperation::CONNECT:
            return static_cast<bool>(document_->topology().connect(operation.from, operation.to));
        case EOperation::DISCONNECT:
            return static_cast<bool>(document_->topology().disconnect(operation.from, operation.to));
        case EOperation::MOVE:
            return static_cast<bool>(document_->layout().set(operation.node, operation.after));
        case EOperation::ACTION:
            return document_->restoreNodeAction(operation.node, operation.action.after);
        }
        return false;
    }

    bool GraphEditingSession::rollbackUndoPrefix(Transaction& transaction, std::size_t first_applied)
    {
        for (std::size_t index = first_applied; index < transaction.operations.size(); ++index)
            if (!applyRedo(transaction.operations[index]))
                return false;
        return true;
    }

    bool GraphEditingSession::rollbackRedoPrefix(Transaction& transaction, std::size_t applied_count)
    {
        while (applied_count != 0U)
        {
            --applied_count;
            if (!applyUndo(transaction.operations[applied_count]))
                return false;
        }
        return true;
    }

    bool GraphEditingSession::undo()
    {
        if (!canUndo() || in_transaction_ || topology_locked_)
            return false;
        try
        {
            redo_.reserve(redo_.size() + 1U);
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
        auto& transaction = undo_.back();
        for (std::size_t index = transaction.operations.size(); index != 0U; --index)
        {
            if (!applyUndo(transaction.operations[index - 1U]))
            {
                if (!rollbackUndoPrefix(transaction, index))
                    poisoned_ = true;
                return false;
            }
        }
        try
        {
            redo_.push_back(std::move(transaction));
            undo_.pop_back();
            if (redo_.back().structural)
                ++revision_;
            return true;
        }
        catch (const std::bad_alloc&)
        {
            if (!rollbackUndoPrefix(transaction, 0U))
                poisoned_ = true;
            return false;
        }
    }

    bool GraphEditingSession::redo()
    {
        if (!canRedo() || in_transaction_ || topology_locked_)
            return false;
        try
        {
            undo_.reserve(undo_.size() + 1U);
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
        auto& transaction = redo_.back();
        std::size_t applied{};
        for (; applied < transaction.operations.size(); ++applied)
        {
            if (!applyRedo(transaction.operations[applied]))
            {
                if (!rollbackRedoPrefix(transaction, applied))
                    poisoned_ = true;
                return false;
            }
        }
        try
        {
            undo_.push_back(std::move(transaction));
            redo_.pop_back();
            if (undo_.back().structural)
                ++revision_;
            return true;
        }
        catch (const std::bad_alloc&)
        {
            if (!rollbackRedoPrefix(transaction, applied))
                poisoned_ = true;
            return false;
        }
    }

    void GraphEditingSession::clear() noexcept
    {
        undo_.clear();
        redo_.clear();
        pending_ = {};
        selected_ = {};
        in_transaction_ = false;
        poisoned_ = false;
    }

    void GraphEditingSession::setTopologyLocked(bool locked) noexcept { topology_locked_ = locked; }
    bool GraphEditingSession::topologyLocked() const noexcept { return topology_locked_; }
    bool GraphEditingSession::poisoned() const noexcept { return poisoned_; }
    bool GraphEditingSession::canUndo() const noexcept { return !poisoned_ && !undo_.empty(); }
    bool GraphEditingSession::canRedo() const noexcept { return !poisoned_ && !redo_.empty(); }
    std::size_t GraphEditingSession::undoDepth() const noexcept { return undo_.size(); }
    std::uint64_t GraphEditingSession::structureRevision() const noexcept { return revision_; }
    graph::NodeId GraphEditingSession::selectedNode() const noexcept { return selected_; }
} // namespace lux::editor::node_graph

#pragma once

#include <lux/engine/editor/node_graph/GraphDocument.hpp>
#include <lux/engine/editor/node_graph/GraphIntent.hpp>
#include <lux/engine/editor/node_graph/visibility.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lux::editor::node_graph
{
    class LUX_NODE_GRAPH_EDITOR_PUBLIC GraphEditingSession final
    {
    public:
        explicit GraphEditingSession(IGraphDocument& document, const IGraphRules& rules) noexcept;

        GraphEditingSession(const GraphEditingSession&) = delete;
        GraphEditingSession& operator=(const GraphEditingSession&) = delete;

        [[nodiscard]] bool apply(const GraphIntent& intent);
        [[nodiscard]] bool beginTransaction(std::string label);
        [[nodiscard]] bool commitTransaction();
        [[nodiscard]] bool undo();
        [[nodiscard]] bool redo();
        void clear() noexcept;

        void setTopologyLocked(bool locked) noexcept;
        [[nodiscard]] bool topologyLocked() const noexcept;
        [[nodiscard]] bool poisoned() const noexcept;
        [[nodiscard]] bool canUndo() const noexcept;
        [[nodiscard]] bool canRedo() const noexcept;
        [[nodiscard]] std::size_t undoDepth() const noexcept;
        [[nodiscard]] std::uint64_t structureRevision() const noexcept;
        [[nodiscard]] graph::NodeId selectedNode() const noexcept;

    private:
        enum class EOperation : std::uint8_t
        {
            ADD,
            REMOVE,
            CONNECT,
            DISCONNECT,
            MOVE,
            ACTION,
        };

        struct Operation final
        {
            EOperation kind{EOperation::MOVE};
            graph::NodeId node;
            graph::PinId from;
            graph::PinId to;
            graph::GraphNodeLayout before;
            graph::GraphNodeLayout after;
            NodeCapture capture;
            NodeActionJournal action;
        };

        struct Transaction final
        {
            std::string label;
            std::vector<Operation> operations;
            bool structural{};
        };

        [[nodiscard]] bool applyConnect(graph::PinId from, graph::PinId to);
        [[nodiscard]] bool applyDisconnect(graph::PinId from, graph::PinId to);
        [[nodiscard]] bool applyAdd(const AddNodeIntent& intent);
        [[nodiscard]] bool applyRemove(graph::NodeId node);
        [[nodiscard]] bool applyMove(const MoveNodeIntent& intent);
        [[nodiscard]] bool applyAction(const InvokeNodeActionIntent& intent);
        [[nodiscard]] bool prepare(std::string label, std::size_t operation_capacity, bool& automatic);
        [[nodiscard]] bool finish(bool automatic);
        [[nodiscard]] bool fail();
        void record(Operation operation);
        [[nodiscard]] bool applyUndo(Operation& operation);
        [[nodiscard]] bool applyRedo(Operation& operation);
        [[nodiscard]] bool rollbackUndoPrefix(Transaction& transaction, std::size_t first_applied);
        [[nodiscard]] bool rollbackRedoPrefix(Transaction& transaction, std::size_t applied_count);

        IGraphDocument* document_{};
        const IGraphRules* rules_{};
        std::vector<Transaction> undo_;
        std::vector<Transaction> redo_;
        Transaction pending_;
        graph::NodeId selected_{};
        std::uint64_t revision_{};
        bool in_transaction_{};
        bool topology_locked_{};
        bool poisoned_{};
    };
} // namespace lux::editor::node_graph

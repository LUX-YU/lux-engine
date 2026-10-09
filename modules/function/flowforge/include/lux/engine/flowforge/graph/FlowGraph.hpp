#pragma once

#include "NodeBase.hpp"
#include <lux/engine/flowforge/script/ScriptGraph.hpp>
#include <lux/engine/function/graph/GraphEdit.hpp>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <vector>

namespace lux::flowforge
{
    struct FlowNodeSnapshot final
    {
        NodeId id;
        std::unique_ptr<Node> node;
        // Input pins followed by output pins; zero requests a new signature pin.
        std::vector<PinId> pins;
    };

    struct FlowNodeInsertion final
    {
        // Invalid ID requests a fresh identity; valid ID explicitly restores a snapshot.
        NodeId id;
        std::unique_ptr<Node>* node{};
        std::span<const PinId> pins;
    };

    /**
     * @class FlowGraph
     * @brief Represents a collection of interconnected Flowforge nodes,
     * Which could be a function or a script.
     */
    class FlowGraph
    {
    public:
        FlowGraph();
        ~FlowGraph();
        FlowGraph(const FlowGraph&) = delete;
        FlowGraph& operator=(const FlowGraph&) = delete;
        FlowGraph(FlowGraph&& other) noexcept;
        FlowGraph& operator=(FlowGraph&& other) noexcept;

        struct NodeEntry final
        {
            NodeId id;
            Node* node;
        };

        [[nodiscard]] auto nodes() const noexcept
        {
            return nodes_ | std::views::transform([](const auto& entry) noexcept
                                                  { return NodeEntry{entry.first, entry.second.get()}; });
        }

        [[nodiscard]] bool addExport(ExportMethodNode exported) noexcept
        {
            exports_.push_back(exported);
            return true;
        }

        [[nodiscard]] bool removeExport(FlowForgeExportNodeId id) noexcept
        {
            for (auto iterator = exports_.begin(); iterator != exports_.end(); ++iterator)
            {
                if (iterator->id == id)
                {
                    exports_.erase(iterator);
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] const std::vector<ExportMethodNode>& exports() const noexcept
        {
            return exports_;
        }

        // Caller validates stable entries before committing its authoring transaction.
        void exchangeExports(std::vector<ExportMethodNode>& exports) noexcept
        {
            exports_.swap(exports);
        }

        [[nodiscard]] NodeId addNode(std::unique_ptr<Node>) noexcept;
        [[nodiscard]] bool insertNode(FlowNodeSnapshot) noexcept;
        [[nodiscard]] Node* findNodeById(NodeId) noexcept;
        [[nodiscard]] const Node* findNodeById(NodeId) const noexcept;
        // Reverse index is derived from the owning store; detached/foreign nodes have no identity here.
        [[nodiscard]] NodeId nodeId(const Node*) const noexcept;
        [[nodiscard]] bool removeNode(NodeId) noexcept;
        [[nodiscard]] std::optional<FlowNodeSnapshot> extractNode(NodeId) noexcept;

        [[nodiscard]] const lux::graph::GraphTopology& topology() const noexcept
        {
            return topology_;
        }

        [[nodiscard]] lux::graph::GraphLayout& layout() noexcept
        {
            return layout_;
        }

        [[nodiscard]] const lux::graph::GraphLayout& layout() const noexcept
        {
            return layout_;
        }

        [[nodiscard]] PinId pinId(const Pin*) const noexcept;
        [[nodiscard]] Pin* findPin(PinId id) noexcept;
        [[nodiscard]] const Pin* findPin(PinId id) const noexcept;
        [[nodiscard]] std::vector<Pin*> linkedPins(PinId id);
        [[nodiscard]] std::vector<const Pin*> linkedPins(PinId id) const;
        // The receiver owns structural mutation; pin payloads are read-only inputs.
        [[nodiscard]] ELinkError connect(const Pin& first, const Pin& second) noexcept;
        [[nodiscard]] ELinkError disconnect(const Pin& first, const Pin& second) noexcept;

        // ------------------------------------------------------------------
        // Graph-local variables. Each variable owns a stable, monotonically
        // increasing id (never recycled) — Get/Set variable nodes reference
        // the id, so renames don't break wiring and serialization can key on
        // it. Storage lives in a host-owned INSTANCE-STATE block laid out by
        // computeStateLayout (StateLayout.hpp); the compiled script accesses
        // variables through a hidden state-pointer argument at those offsets.
        // ------------------------------------------------------------------
        struct GraphVariable
        {
            uint64_t id;
            std::string name;
            const lux::meta::RefType* type;
            lux::meta::RuntimeObject default_value;
        };

        [[nodiscard]] uint64_t nextVariableId() const noexcept
        {
            return next_var_id_;
        }

        // A transactional candidate/replay must not recycle IDs already issued by its source.
        void preserveIssuedIdsFrom(const FlowGraph& source) noexcept
        {
            topology_.preserveIssuedIdsFrom(source.topology_);
            if (source.next_var_id_ > next_var_id_)
            {
                next_var_id_ = source.next_var_id_;
            }
        }

        // Transactional authoring commits validated storage; IDs remain monotonic across replay.
        void exchangeVariables(std::vector<GraphVariable>& variables) noexcept
        {
            for (const auto& variable : variables)
            {
                if (variable.id >= next_var_id_)
                {
                    next_var_id_ = variable.id + 1;
                }
            }
            variables_.swap(variables);
        }

        uint64_t addVariable(std::string name, const lux::meta::RefType* type, lux::meta::RuntimeObject default_value)
        {
            if (next_var_id_ == UINT64_MAX)
            {
                return 0;
            }
            const uint64_t id = next_var_id_++;
            variables_.push_back(GraphVariable{id, std::move(name), type, std::move(default_value)});
            return id;
        }

        /**
         * @brief Decode path: adds a variable KEEPING the given (serialized)
         *        id and bumps the counter past it. Returns false if the id
         *        is already taken.
         */
        bool addVariableWithId(
            uint64_t id,
            std::string name,
            const lux::meta::RefType* type,
            lux::meta::RuntimeObject default_value
        )
        {
            if (id == 0 || id == UINT64_MAX || findVariable(id))
            {
                return false;
            }
            variables_.push_back(GraphVariable{id, std::move(name), type, std::move(default_value)});
            if (id >= next_var_id_)
            {
                next_var_id_ = id + 1;
            }
            return true;
        }

        bool removeVariable(uint64_t id)
        {
            for (auto it = variables_.begin(); it != variables_.end(); ++it)
            {
                if (it->id == id)
                {
                    variables_.erase(it);
                    return true;
                }
            }
            return false;
        }

        GraphVariable* findVariable(uint64_t id)
        {
            for (auto& v : variables_)
            {
                if (v.id == id)
                {
                    return &v;
                }
            }
            return nullptr;
        }

        const GraphVariable* findVariable(uint64_t id) const
        {
            for (auto& v : variables_)
            {
                if (v.id == id)
                {
                    return &v;
                }
            }
            return nullptr;
        }

        const std::vector<GraphVariable>& variables() const
        {
            return variables_;
        }

        std::vector<GraphVariable>& variables()
        {
            return variables_;
        }

    private:
        friend class FlowGraphEdit;
        friend class Node;
        // Dynamic pin construction/restoration updates topology through these internal operations.
        [[nodiscard]] bool registerPin(Pin& pin, PinId restored = {}) noexcept;
        void unregisterPin(Pin& pin) noexcept;
        [[nodiscard]] bool attachNodeStructure(NodeId id, Node& node, std::span<const PinId> pins) noexcept;
        [[nodiscard]] std::vector<PinId> snapshotPins(const Node&) const;
        void forgetPins(const Node&) noexcept;
        void rebindNodes() noexcept;

        std::vector<GraphVariable> variables_;
        std::vector<ExportMethodNode> exports_;
        uint64_t next_var_id_{1};
        std::map<NodeId, std::unique_ptr<Node>> nodes_;
        std::unordered_map<const Node*, NodeId> node_ids_;
        std::unordered_map<PinId, Pin*> pin_store_;
        std::unordered_map<const Pin*, PinId> pin_ids_;
        lux::graph::GraphTopology topology_;
        lux::graph::GraphLayout layout_;
    };

    struct FlowGraphChange final
    {
        // Ownership changes only at commit. Failure leaves every pointed-to unique_ptr intact.
        std::span<const FlowNodeInsertion> insert;
        std::span<const NodeId> erase;
        std::span<const lux::graph::LinkRecord> connect, disconnect;
        std::span<const lux::graph::GraphLayoutEntry> place;
        std::span<const NodeId> unplace;
    };

    class FlowGraphEdit final
    {
    public:
        using Result = lux::cxx::expected<FlowGraphEdit, lux::graph::GraphTopologyFailure>;
        [[nodiscard]] static Result prepare(FlowGraph&, const FlowGraphChange&);
        FlowGraphEdit(FlowGraphEdit&&) noexcept;
        FlowGraphEdit(const FlowGraphEdit&) = delete;
        FlowGraphEdit& operator=(const FlowGraphEdit&) = delete;
        ~FlowGraphEdit();

        [[nodiscard]] std::span<const NodeId> insertedIds() const noexcept;
        [[nodiscard]] std::span<const std::pair<Pin*, PinId>> assignedPins() const noexcept;
        [[nodiscard]] lux::cxx::expected<void, lux::graph::GraphTopologyFailure> place(
            NodeId,
            lux::graph::GraphNodeLayout
        );
        void commit() noexcept;
        // Removed nodes are detached and retained until the journal adopts them or this plan is destroyed.
        [[nodiscard]] std::vector<FlowNodeSnapshot> takeRemoved() noexcept;

    private:
        explicit FlowGraphEdit(FlowGraph&);

        struct Insertion final
        {
            std::unique_ptr<Node>* source;
            NodeId id;
        };

        FlowGraph* target_;
        lux::graph::GraphEdit structure_;
        std::map<NodeId, std::unique_ptr<Node>> nodes_;
        std::unordered_map<const Node*, NodeId> node_ids_;
        std::unordered_map<PinId, Pin*> pin_store_;
        std::unordered_map<const Pin*, PinId> pin_ids_;
        std::vector<Insertion> insert_;
        std::vector<std::pair<Pin*, PinId>> pins_;
        std::vector<NodeId> inserted_ids_;
        std::vector<NodeId> keep_, erase_;
        std::vector<FlowNodeSnapshot> removed_;
        bool storage_changed_{};
    };
} // namespace lux::flowforge

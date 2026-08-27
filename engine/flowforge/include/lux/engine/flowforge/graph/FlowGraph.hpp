#pragma once

#include <vector>
#include <memory>
#include "NodeBase.hpp"
#include <lux/cxx/container/SparseSet.hpp>

namespace lux::flowforge
{
	using NodeUserDataPtr = std::unique_ptr<void, void (*)(void*)>;

    struct NodeStorage
    {
        std::unique_ptr<Node> node;
		size_t                index;
        NodeUserDataPtr       user_data; // Optional user data for the node
    };

    /**
     * @class FlowGraph
     * @brief Represents a collection of interconnected Flowforge nodes,
     * Which could be a function or a script.
     */
    class FlowGraph
    {
        // This’s a kind of compromise
        static constexpr size_t set_offset = 1;
    public:
        FlowGraph(){}

        ~FlowGraph() = default; // Nodes are owned by unique_ptr, will be cleaned up
        FlowGraph(const FlowGraph&) = delete;
        FlowGraph& operator=(const FlowGraph&) = delete;
        FlowGraph(FlowGraph&&) = default;
        FlowGraph& operator=(FlowGraph&&) = default;


        // Defined out-of-line in FlowGraph.cpp: forces MSVC to generate an exported
        // copy in flowforged.dll. Without this, an inline-in-header definition on a
        // class produces no export symbol, and DLL consumers
        // (e.g. engine::flowforge_compiler) get LNK2019.
        const std::vector<NodeStorage>& nodes() const;

        // Every node entering the graph is re-keyed with a STABLE id from
        // the graph's monotonic counter (never recycled) — the pointer-based
        // ids the convenience constructors self-assign are process-local.
        // Pin ids are re-derived from the node id (see makePinId).
        size_t addNodes(std::unique_ptr<Node> node) {
            return addNodes(std::move(node), NodeUserDataPtr(nullptr, [](void*) {}));
        }

        size_t addNodes(std::unique_ptr<Node> node, NodeUserDataPtr user_data) {
            node->assignStableId(next_node_id_++);
            auto idx = nodes_.emplace(
                std::move(node), 0, std::move(user_data)
            );
			nodes_.at(idx).index = idx;
            return idx;
        }

        /**
         * @brief Decode path: adds a node KEEPING the given (serialized)
         *        stable id and bumps the counter past it, so later addNodes
         *        can never mint a duplicate.
         */
        size_t addNodesWithId(uint64_t stable_id, std::unique_ptr<Node> node) {
            node->assignStableId(stable_id);
            if (stable_id >= next_node_id_)
                next_node_id_ = stable_id + 1;
            auto idx = nodes_.emplace(
                std::move(node), 0, NodeUserDataPtr(nullptr, [](void*) {})
            );
            nodes_.at(idx).index = idx;
            return idx;
        }

        /**
         * @brief Finds a live node by its STABLE id (linear scan — decode /
         *        error-reporting path, not a hot path).
         */
        Node* findNodeById(uint64_t stable_id) {
            for (auto& storage : nodes_.values())
                if (storage.node && storage.node->id() == stable_id)
                    return storage.node.get();
            return nullptr;
        }
        const Node* findNodeById(uint64_t stable_id) const {
            for (auto& storage : nodes_.values())
                if (storage.node && storage.node->id() == stable_id)
                    return storage.node.get();
            return nullptr;
        }

		bool removeNode(size_t index) {
            if (!nodes_.contains(index))
            {
                return false;
            }
            nodes_.erase(index);
            return true;
		}

        /**
         * @brief Detaches a node, MOVING its storage (node + user_data) out intact.
         *        Unlike removeNode the node object survives — the caller owns it and
         *        can restore it later via insertNodeAt (undo). The caller must drop
         *        the node's links first; the index is recycled like removeNode.
         * @return True if the index was live and the storage was moved out.
         */
        bool extractNode(size_t index, NodeStorage& out) {
            return nodes_.extract(index, out);
        }

        /**
         * @brief Restores a node at a CALLER-CHOSEN index. Use when other data still
         *        references that index (e.g. undoing a removeNode: links and editor
         *        refs are keyed by node index, so the node must come back under its
         *        ORIGINAL index). Reconciles the recycling allocator so the index can
         *        never be handed out again by a later addNodes.
         * @return True if the index was free and the node was inserted.
         */
        bool insertNodeAt(size_t index, std::unique_ptr<Node> node) {
            return insertNodeAt(index, std::move(node), NodeUserDataPtr(nullptr, [](void*) {}));
        }

        bool insertNodeAt(size_t index, std::unique_ptr<Node> node, NodeUserDataPtr user_data) {
            if (!nodes_.try_emplace_at(index, std::move(node), 0, std::move(user_data)))
            {
                return false;
            }
            nodes_.at(index).index = index;
            return true;
        }

		bool hasNode(size_t index) const {
			return nodes_.contains(index);
		}

        NodeStorage& getNode(size_t idx) {
            return nodes_.at(idx);
        }

        const NodeStorage& getNode(size_t idx) const {
            return nodes_.at(idx);
        }

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
            uint64_t                  id;
            std::string               name;
            const lux::meta::RefType* type;
            lux::meta::RuntimeObject  default_value;
        };

        uint64_t addVariable(std::string name, const lux::meta::RefType* type,
                             lux::meta::RuntimeObject default_value)
        {
            const uint64_t id = next_var_id_++;
            variables_.push_back(GraphVariable{
                id, std::move(name), type, std::move(default_value)});
            return id;
        }

        /**
         * @brief Decode path: adds a variable KEEPING the given (serialized)
         *        id and bumps the counter past it. Returns false if the id
         *        is already taken.
         */
        bool addVariableWithId(uint64_t id, std::string name,
                               const lux::meta::RefType* type,
                               lux::meta::RuntimeObject default_value)
        {
            if (id == 0 || findVariable(id))
                return false;
            variables_.push_back(GraphVariable{
                id, std::move(name), type, std::move(default_value)});
            if (id >= next_var_id_)
                next_var_id_ = id + 1;
            return true;
        }

        bool removeVariable(uint64_t id)
        {
            for (auto it = variables_.begin(); it != variables_.end(); ++it)
            {
                if (it->id == id) { variables_.erase(it); return true; }
            }
            return false;
        }

        GraphVariable* findVariable(uint64_t id)
        {
            for (auto& v : variables_)
                if (v.id == id) return &v;
            return nullptr;
        }
        const GraphVariable* findVariable(uint64_t id) const
        {
            for (auto& v : variables_)
                if (v.id == id) return &v;
            return nullptr;
        }

        const std::vector<GraphVariable>& variables() const { return variables_; }
        std::vector<GraphVariable>&       variables()       { return variables_; }

    private:
        std::vector<GraphVariable>              variables_;
        uint64_t                                next_var_id_{1};
        // Monotonic STABLE node-id counter (never recycled; serialized ids
        // bump it past themselves via addNodesWithId).
        uint64_t                                next_node_id_{1};
        // start from one
		lux::cxx::AutoSparseSet<NodeStorage, 1> nodes_;
    };

} // namespace lux::flowforge

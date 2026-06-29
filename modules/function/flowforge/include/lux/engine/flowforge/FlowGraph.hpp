#pragma once

#include <vector>
#include <memory>
#include "NodeBase.hpp"
#include <lux/cxx/container/SparseSet.hpp>
#include <lux/engine/function/visibility.h>

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
    class LUX_FUNCTION_PUBLIC FlowGraph
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
        // LUX_FUNCTION_PUBLIC class produces no export symbol, and DLL consumers
        // (e.g. engine::flowforge_compiler) get LNK2019.
        const std::vector<NodeStorage>& nodes() const;

        size_t addNodes(std::unique_ptr<Node> node) {
            auto idx = nodes_.emplace(
                std::move(node), 0, NodeUserDataPtr(nullptr, [](void*) {})
            );
            nodes_.at(idx).index = idx;
            return idx;
        }

        size_t addNodes(std::unique_ptr<Node> node, NodeUserDataPtr user_data) {
            auto idx = nodes_.emplace(
                std::move(node), 0, std::move(user_data)
            );
			nodes_.at(idx).index = idx;
            return idx;
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

        bool createObject(const std::string& name, const lux::meta::RefType* type) {
            auto [it, inserted] = scoped_variables_.try_emplace(name, type);
            return inserted;
        }

        template<typename T>
        bool createObject(const std::string& name, T&& value) {
            auto [it, inserted] = scoped_variables_.try_emplace(name, std::forward<T>(value));
            return inserted;
        }

    private:
		using RuntimeObjectMap = std::unordered_map<std::string, lux::meta::RuntimeObject>;

        RuntimeObjectMap                        scoped_variables_;
        // start from one
		lux::cxx::AutoSparseSet<NodeStorage, 1> nodes_;
    };

} // namespace lux::flowforge
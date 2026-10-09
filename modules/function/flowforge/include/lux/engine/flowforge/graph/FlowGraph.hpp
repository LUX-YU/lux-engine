#pragma once

#include <lux/engine/flowforge/graph/FlowNode.hpp>
#include <lux/engine/flowforge/script/ScriptGraph.hpp>
#include <lux/engine/function/graph/GraphEdit.hpp>

#include <map>
#include <ranges>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::flowforge
{
    class FlowGraphEdit;

    class LUX_ENGINE_FLOWFORGE_PUBLIC FlowGraph final
    {
    public:
        FlowGraph() noexcept;
        ~FlowGraph();
        FlowGraph(const FlowGraph&) = delete;
        FlowGraph& operator=(const FlowGraph&) = delete;
        FlowGraph(FlowGraph&&) noexcept;
        FlowGraph& operator=(FlowGraph&&) noexcept;

        [[nodiscard]] FlowGraphResult<NodeId> addNode(FlowNode) noexcept;
        [[nodiscard]] FlowGraphResult<NodeId> addNodeWithId(
            NodeId,
            FlowNode,
            std::span<const FlowPinEntry> = {}
        ) noexcept;
        [[nodiscard]] FlowGraphResult<FlowNodeSnapshot> extractNode(NodeId) noexcept;
        [[nodiscard]] FlowGraphResult<void> removeNode(NodeId) noexcept;
        [[nodiscard]] const FlowNode* node(NodeId) const noexcept;
        [[nodiscard]] const FlowPinPayload* pin(PinId) const noexcept;
        [[nodiscard]] FlowPinPayload* pin(PinId) noexcept;
        [[nodiscard]] PinId pinId(NodeId, graph::PinSemanticId) const noexcept;
        // Interactive endpoints may arrive in either order; topology stores output -> input.
        [[nodiscard]] FlowGraphResult<void> connect(PinId from, PinId to) noexcept;
        [[nodiscard]] FlowGraphResult<void> disconnect(PinId from, PinId to) noexcept;

        [[nodiscard]] auto nodes() const noexcept
        {
            return std::views::transform(
                nodes_,
                [](const auto& entry) noexcept
                { return std::pair<NodeId, const FlowNode*>{entry.first, &entry.second}; }
            );
        }

        [[nodiscard]] const graph::GraphTopology& topology() const noexcept
        {
            return topology_;
        }

        [[nodiscard]] const graph::GraphLayout& layout() const noexcept
        {
            return layout_;
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
            const bool is_invalid_id = id == 0 || id == UINT64_MAX;
            const bool has_duplicate = findVariable(id) != nullptr;
            if (is_invalid_id || has_duplicate)
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
        using NodeStorage = std::map<NodeId, FlowNode>;
        using PinStorage = std::unordered_map<PinId, FlowPinPayload>;

        std::vector<GraphVariable> variables_;
        std::vector<ExportMethodNode> exports_;
        std::uint64_t next_var_id_{1};
        NodeStorage nodes_;
        PinStorage pins_;
        graph::GraphTopology topology_;
        graph::GraphLayout layout_;
    };

    struct FlowNodeEntry final
    {
        NodeId id;
        const FlowNode* value{};
        std::span<const FlowPinEntry> pins;
    };

    struct FlowGraphChange final
    {
        std::span<const FlowNodeEntry> insert;
        std::span<const NodeId> erase;
        std::span<const graph::LinkRecord> connect, disconnect;
        std::span<const graph::GraphLayoutEntry> place;
        std::span<const NodeId> unplace;
    };

    struct FlowNodeAssignment final
    {
        NodeId id;
        std::vector<graph::PinRecord> pins;
    };

    // Uses the shared GraphEdit for all structural preparation. Domain values are prepared
    // beforehand and transferred by node handles at commit; no callback or value destruction
    // runs in the commit interval. Removed snapshots retain their provider until pin cleanup.
    class LUX_ENGINE_FLOWFORGE_PUBLIC FlowGraphEdit final
    {
    public:
        [[nodiscard]] static FlowGraphResult<FlowGraphEdit> prepare(FlowGraph&, const FlowGraphChange&) noexcept;
        ~FlowGraphEdit();
        FlowGraphEdit(FlowGraphEdit&&) noexcept;
        FlowGraphEdit(const FlowGraphEdit&) = delete;
        FlowGraphEdit& operator=(const FlowGraphEdit&) = delete;
        FlowGraphEdit& operator=(FlowGraphEdit&&) = delete;

        [[nodiscard]] std::span<const FlowNodeAssignment> insertedNodes() const noexcept;
        [[nodiscard]] FlowGraphResult<void> place(NodeId, graph::GraphNodeLayout) noexcept;
        void commit() noexcept;
        [[nodiscard]] std::vector<FlowNodeSnapshot> takeRemoved() noexcept;

    private:
        friend class FlowGraph;
        using NodeStorage = FlowGraph::NodeStorage;
        using PinStorage = FlowGraph::PinStorage;
        explicit FlowGraphEdit(FlowGraph&) noexcept;
        [[nodiscard]] FlowGraphResult<void> insert(NodeId, FlowNode, std::span<const FlowPinEntry>) noexcept;
        [[nodiscard]] FlowGraphResult<void> finishPins() noexcept;
        void reserveCommit() noexcept;

        FlowGraph* target_;
        graph::GraphEdit structure_;
        NodeStorage staged_nodes_;
        std::vector<FlowNodeSnapshot> removed_;
        PinStorage staged_pins_;
        std::vector<std::vector<FlowPinEntry>> inserted_pins_;
        std::vector<FlowNodeAssignment> inserted_;
        bool committed_{};
    };
} // namespace lux::flowforge

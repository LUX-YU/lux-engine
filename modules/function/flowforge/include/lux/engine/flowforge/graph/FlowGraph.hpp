#pragma once

#include "NodeBase.hpp"
#include <lux/cxx/container/SparseSet.hpp>
#include <lux/engine/flowforge/script/ScriptGraph.hpp>
#include <lux/engine/function/graph/GraphLayout.hpp>
#include <memory>
#include <new>
#include <vector>

namespace lux::flowforge
{
struct NodeStorage
{
    std::unique_ptr<Node> node;
    size_t index;
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
    FlowGraph();
    ~FlowGraph();
    FlowGraph(const FlowGraph &) = delete;
    FlowGraph &operator=(const FlowGraph &) = delete;
    FlowGraph(FlowGraph &&other) noexcept;
    FlowGraph &operator=(FlowGraph &&other) noexcept;

    // Defined out-of-line in FlowGraph.cpp: forces MSVC to generate an exported
    // copy in flowforged.dll. Without this, an inline-in-header definition on a
    // class produces no export symbol, and DLL consumers
    // (e.g. engine::flowforge_compiler) get LNK2019.
    const std::vector<NodeStorage> &nodes() const;

    [[nodiscard]] bool addExport(ExportMethodNode exported) noexcept
    {
        try
        {
            exports_.push_back(exported);
            return true;
        }
        catch (const std::bad_alloc &)
        {
            return false;
        }
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

    [[nodiscard]] const std::vector<ExportMethodNode> &exports() const noexcept { return exports_; }

    // Caller validates stable entries before committing its authoring transaction.
    void exchangeExports(std::vector<ExportMethodNode> &exports) noexcept { exports_.swap(exports); }

    size_t addNodes(std::unique_ptr<Node> node);

    /**
     * @brief Decode path: adds a node KEEPING the given (serialized)
     *        stable id and bumps the counter past it, so later addNodes
     *        can never mint a duplicate.
     */
    size_t addNodesWithId(NodeId stable_id, std::unique_ptr<Node> node);

    /**
     * @brief Finds a live node by its STABLE id (linear scan — decode /
     *        error-reporting path, not a hot path).
     */
    Node *findNodeById(NodeId stable_id);
    const Node *findNodeById(NodeId stable_id) const;

    bool removeNode(size_t index);

    /**
     * @brief Detaches a node, moving its semantic storage out intact.
     *        Unlike removeNode the node object survives — the caller owns it and
     *        can restore it later via insertNodeAt (undo). The caller must drop
     *        the node's links first; the index is recycled like removeNode.
     * @return True if the index was live and the storage was moved out.
     */
    bool extractNode(size_t index, NodeStorage &out);

    /**
     * @brief Restores a node at a CALLER-CHOSEN index. Use when other data still
     *        references that index (e.g. undoing a removeNode: links and editor
     *        refs are keyed by node index, so the node must come back under its
     *        ORIGINAL index). Reconciles the recycling allocator so the index can
     *        never be handed out again by a later addNodes.
     * @return True if the index was free and the node was inserted.
     */
    bool insertNodeAt(size_t index, std::unique_ptr<Node> node);

    bool hasNode(size_t index) const { return nodes_.contains(index); }

    NodeStorage &getNode(size_t idx) { return nodes_.at(idx); }

    const NodeStorage &getNode(size_t idx) const { return nodes_.at(idx); }

    [[nodiscard]] lux::graph::GraphTopology &topology() noexcept { return topology_; }
    [[nodiscard]] const lux::graph::GraphTopology &topology() const noexcept { return topology_; }
    [[nodiscard]] lux::graph::GraphLayout &layout() noexcept { return layout_; }
    [[nodiscard]] const lux::graph::GraphLayout &layout() const noexcept { return layout_; }

    [[nodiscard]] Pin *findPin(PinId id) noexcept;
    [[nodiscard]] const Pin *findPin(PinId id) const noexcept;
    [[nodiscard]] std::vector<Pin *> linkedPins(PinId id);
    [[nodiscard]] std::vector<const Pin *> linkedPins(PinId id) const;
    [[nodiscard]] ELinkError connect(Pin &first, Pin &second) noexcept;
    [[nodiscard]] ELinkError disconnect(Pin &first, Pin &second) noexcept;
    [[nodiscard]] bool registerPin(Pin &pin) noexcept;
    void unregisterPin(Pin &pin) noexcept;
    [[nodiscard]] bool assignPinId(Pin &pin, PinId id) noexcept;
    // Source reconstruction assigns persisted IDs before the node is attached.
    [[nodiscard]] static bool assignDetachedPinId(Pin &, PinId) noexcept;

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
        const lux::meta::RefType *type;
        lux::meta::RuntimeObject default_value;
    };

    [[nodiscard]] uint64_t nextVariableId() const noexcept { return next_var_id_; }
    // Transactional authoring commits validated storage; IDs remain monotonic across replay.
    void exchangeVariables(std::vector<GraphVariable> &variables) noexcept
    {
        for (const auto &variable : variables)
        {
            if (variable.id >= next_var_id_)
            {
                next_var_id_ = variable.id + 1;
            }
        }
        variables_.swap(variables);
    }

    uint64_t addVariable(std::string name, const lux::meta::RefType *type, lux::meta::RuntimeObject default_value)
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
    bool addVariableWithId(uint64_t id, std::string name, const lux::meta::RefType *type,
                           lux::meta::RuntimeObject default_value)
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

    GraphVariable *findVariable(uint64_t id)
    {
        for (auto &v : variables_)
        {
            if (v.id == id)
            {
                return &v;
            }
        }
        return nullptr;
    }
    const GraphVariable *findVariable(uint64_t id) const
    {
        for (auto &v : variables_)
        {
            if (v.id == id)
            {
                return &v;
            }
        }
        return nullptr;
    }

    const std::vector<GraphVariable> &variables() const { return variables_; }
    std::vector<GraphVariable> &variables() { return variables_; }

  private:
    friend class FlowGraphEdit;
    [[nodiscard]] bool attachNodeStructure(Node &node, bool preserve_pin_ids) noexcept;
    void rebindNodes() noexcept;

    std::vector<GraphVariable> variables_;
    std::vector<ExportMethodNode> exports_;
    uint64_t next_var_id_{1};
    // start from one
    lux::cxx::AutoSparseSet<NodeStorage, 1> nodes_;
    lux::graph::GraphTopology topology_;
    lux::graph::GraphLayout layout_;
};

struct FlowGraphChange final
{
    // Ownership changes only at commit. Failure leaves every pointed-to unique_ptr intact.
    std::span<std::unique_ptr<Node> *const> insert;
    std::span<const NodeId> erase;
    std::span<const lux::graph::LinkRecord> connect, disconnect;
    std::span<const lux::graph::GraphLayoutEntry> place;
    std::span<const NodeId> unplace;
    bool preserve_insert_ids{true};
};

class FlowGraphEdit final
{
  public:
    using Result = lux::cxx::expected<FlowGraphEdit, lux::graph::GraphTopologyFailure>;
    [[nodiscard]] static Result prepare(FlowGraph &, const FlowGraphChange &);
    FlowGraphEdit(FlowGraphEdit &&) noexcept;
    FlowGraphEdit(const FlowGraphEdit &) = delete;
    FlowGraphEdit &operator=(const FlowGraphEdit &) = delete;
    ~FlowGraphEdit();

    [[nodiscard]] std::span<const NodeId> insertedIds() const noexcept;
    [[nodiscard]] std::span<const std::pair<Pin *, PinId>> assignedPins() const noexcept;
    [[nodiscard]] lux::cxx::expected<void, lux::graph::GraphTopologyFailure> place(NodeId, lux::graph::GraphNodeLayout);
    void commit() noexcept;
    // Removed nodes are detached and retained until the journal adopts them or this plan is destroyed.
    [[nodiscard]] std::vector<std::unique_ptr<Node>> takeRemoved() noexcept;

  private:
    explicit FlowGraphEdit(FlowGraph &);
    struct Insertion final
    {
        std::unique_ptr<Node> *source;
        std::size_t index;
        NodeId id;
    };
    FlowGraph *target_;
    lux::graph::GraphTopology topology_;
    lux::graph::GraphLayout layout_;
    lux::cxx::AutoSparseSet<NodeStorage, 1> nodes_;
    std::vector<Insertion> insert_;
    std::vector<std::pair<Pin *, PinId>> pins_;
    std::vector<NodeId> inserted_ids_;
    std::vector<std::size_t> keep_, erase_;
    std::vector<std::unique_ptr<Node>> removed_;
    bool topology_changed_{}, layout_changed_{}, storage_changed_{};
};
} // namespace lux::flowforge

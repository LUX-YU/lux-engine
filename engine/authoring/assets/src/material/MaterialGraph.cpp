// =============================================================================
//  MaterialGraph.cpp  —  Container implementation (pure data model; lowering
//  logic lives at the function layer, in Lowering.cpp)
// -----------------------------------------------------------------------------
//  Type convention: an input pin's type is authoritative (used for type checking
//  and for the type of the materialized constant when unconnected); an output
//  pin's type is only an editor hint — lowering derives the real result type
//  from the node's payload (see Lowering.cpp).
// =============================================================================

#include <lux/engine/authoring/assets/material/MaterialGraph.hpp>
#include <lux/engine/authoring/assets/material/Nodes.hpp>

#include <utility>

namespace lux::rdesc
{
    // ---- Container ------------------------------------------------------------

    MaterialGraph::MaterialGraph() = default;
    MaterialGraph::~MaterialGraph() = default;
    MaterialGraph::MaterialGraph(MaterialGraph&&) noexcept = default;
    MaterialGraph& MaterialGraph::operator=(MaterialGraph&&) noexcept = default;

    MaterialGraph MaterialGraph::clone() const
    {
        MaterialGraph g;
        g.shading_model = shading_model;
        g.texture_slots = texture_slots;
        g.param_slots   = param_slots;
        g.render_state  = render_state;
        g.next_id_      = next_id_;
        // Polymorphic deep copy, preserving original ids (connections reference nodes by id).
        for (const auto& [id, n] : nodes_)
            if (n) g.nodes_.emplace(id, n->clone());
        return g;
    }

    node_id MaterialGraph::addNode(std::unique_ptr<Node> node)
    {
        if (!node)
            return invalid_node;

        const node_id id = next_id_++;
        node->setId(id);
        nodes_.emplace(id, std::move(node));
        return id;
    }

    node_id MaterialGraph::addNodeWithId(node_id id, std::unique_ptr<Node> node)
    {
        if (!node || id == invalid_node || nodes_.find(id) != nodes_.end())
            return invalid_node;
        node->setId(id);
        nodes_.emplace(id, std::move(node));
        if (id >= next_id_)
            next_id_ = id + 1;
        return id;
    }

    std::unique_ptr<Node> MaterialGraph::extractNode(node_id id)
    {
        auto it = nodes_.find(id);
        if (it == nodes_.end())
            return nullptr;
        auto out = std::move(it->second);
        nodes_.erase(it);
        return out;
    }

    Node* MaterialGraph::node(node_id id) noexcept
    {
        auto it = nodes_.find(id);
        return it == nodes_.end() ? nullptr : it->second.get();
    }

    const Node* MaterialGraph::node(node_id id) const noexcept
    {
        auto it = nodes_.find(id);
        return it == nodes_.end() ? nullptr : it->second.get();
    }

    void MaterialGraph::removeNode(node_id id)
    {
        // Clear every input connection that points at this node, to avoid dangling sources.
        for (auto& [other_id, other] : nodes_)
        {
            if (other_id == id)
                continue;
            for (DataPin& pin : other->inputs())
            {
                if (pin.source.node == id)
                    pin.source = PinLink{};
            }
        }
        nodes_.erase(id);
    }

    bool MaterialGraph::connect(node_id src, uint32_t src_pin, node_id dst, uint32_t dst_pin)
    {
        Node* s = node(src);
        Node* d = node(dst);
        if (!s || !d)
            return false;
        if (src_pin >= s->outputs().size())
            return false;
        if (dst_pin >= d->inputs().size())
            return false;

        // Type-compatibility checking is left to lowerToIR (validating at connect time is the editor's job).
        d->inputs()[dst_pin].source = PinLink{ src, src_pin };
        return true;
    }

    void MaterialGraph::disconnect(node_id dst, uint32_t dst_pin)
    {
        Node* d = node(dst);
        if (!d || dst_pin >= d->inputs().size())
            return;
        d->inputs()[dst_pin].source = PinLink{};
    }

} // namespace lux::rdesc

// =============================================================================
//  MaterialGraph.cpp  —  容器实现（纯数据模型；降级逻辑见 function 层 Lowering.cpp）
// -----------------------------------------------------------------------------
//  类型约定：输入引脚类型权威（类型检查 + 未连接时物化常量的类型）；输出引脚类型
//  仅作编辑器提示——lowering 从节点 payload 推导真实产出类型（见 Lowering.cpp）。
// =============================================================================

#include <lux/engine/description/material_graph/MaterialGraph.hpp>
#include <lux/engine/description/material_graph/Nodes.hpp>

#include <utility>

namespace lux::rdesc
{
    // ---- 容器 ---------------------------------------------------------------

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
        // 多态深拷贝，保留原 id（连接按 id 引用）。
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
        // 清掉所有指向该节点的输入连接，避免悬空 source。
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

        // 类型兼容性校验留给 lowerToIR（连接期校验是编辑器层的事）。
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

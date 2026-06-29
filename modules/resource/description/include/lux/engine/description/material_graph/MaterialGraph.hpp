#pragma once
// =============================================================================
//  MaterialGraph.hpp  —  材质图容器（纯数据模型）
// -----------------------------------------------------------------------------
//  下沉到 description 层：资产直接持有它（具体数据结构，非不透明 blob），编辑器从中
//  克隆出可编辑副本。图 -> MatIR 的【降级】逻辑在 function 层
//  (`<lux/engine/material_graph/Lowering.hpp>` 的 lowerToIR)。
// =============================================================================

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Node.hpp"
#include <lux/engine/description/MaterialGraphContract.hpp>
#include <lux/engine/resource/visibility.h>

namespace lux::rdesc
{
    /// 图声明的纹理槽（-> ShadingModelDescriptor + 描述符布局 set 2）。
    struct TextureSlotDecl
    {
        std::string name;
    };

    /// 图声明的标量/向量参数（-> 材质 SSBO set 4）。
    struct ParamSlotDecl
    {
        std::string   name;
        EMatValueType type    = EMatValueType::Float;
        float         dflt[4] = { 0, 0, 0, 0 };
    };

    /// 渲染状态（管线相关，非表面属性）：alpha 混合模式 / cutout 阈值 / 双面。
    /// 复用既有 EAlphaMode（与内建材质同一枚举）。Mask 由烘焙期在 frag 里 emit
    /// `discard`；Blend/双面 是 PSO 状态（进 bucket key）。
    struct RenderState
    {
        EAlphaMode alpha_mode   = EAlphaMode::Opaque;
        float      alpha_cutoff = 0.5f;
        bool       double_sided = false;
    };

    class LUX_RESOURCE_PUBLIC MaterialGraph
    {
    public:
        MaterialGraph();
        ~MaterialGraph();
        MaterialGraph(const MaterialGraph&) = delete;
        MaterialGraph& operator=(const MaterialGraph&) = delete;
        MaterialGraph(MaterialGraph&&) noexcept;
        MaterialGraph& operator=(MaterialGraph&&) noexcept;

        /// 深拷贝（图是 move-only：节点是 unique_ptr 多态对象）。保留节点 id +
        /// 全部连接/常量 + 槽声明 + 渲染状态。编辑器用它从资产持有的图复制出工作副本。
        [[nodiscard]] MaterialGraph clone() const;

        node_id     addNode(std::unique_ptr<Node> node);
        Node*       node(node_id id) noexcept;
        const Node* node(node_id id) const noexcept;
        void        removeNode(node_id id);

        /// 以指定 id 插入（GraphKit undo 的 id 稳定契约：恢复被删节点必须用回
        /// 原 id，连线与记录的意图都按 id 引用）。id 被占用 / 无效 / node 为空
        /// 时返回 invalid_node；next_id_ 取高水位，保证后续 addNode 不冲突。
        node_id addNodeWithId(node_id id, std::unique_ptr<Node> node);

        /// 摘除节点但不销毁、也不触碰其它节点的输入连接（编辑器在摘除前已逐条
        /// 断开记录过的连线）。不存在时返回 nullptr。
        [[nodiscard]] std::unique_ptr<Node> extractNode(node_id id);

        /// 连接 src 的输出 src_pin -> dst 的输入 dst_pin。返回是否成功。
        bool connect(node_id src, uint32_t src_pin, node_id dst, uint32_t dst_pin);
        void disconnect(node_id dst, uint32_t dst_pin);

        const std::unordered_map<node_id, std::unique_ptr<Node>>& nodes() const noexcept
        {
            return nodes_;
        }

        EMaterialShadingModel        shading_model = EMaterialShadingModel::PbrMetallicRoughness;
        std::vector<TextureSlotDecl> texture_slots;
        std::vector<ParamSlotDecl>   param_slots;
        RenderState                  render_state;

    private:
        std::unordered_map<node_id, std::unique_ptr<Node>> nodes_;
        node_id                                            next_id_ = 1;
    };

} // namespace lux::rdesc

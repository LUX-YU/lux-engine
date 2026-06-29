#pragma once
// =============================================================================
//  Nodes.hpp  —  具体材质节点（携带 payload）
// -----------------------------------------------------------------------------
//  每个节点的构造函数负责按契约建立自己的输入/输出引脚（见 Node.cpp）。
//  M1 走通骨架只需要 ConstantNode + OutputSurfaceNode；其余为 M2 预置。
// =============================================================================

#include <memory>

#include "Node.hpp"

namespace lux::rdesc
{
    enum class EMathOp : uint8_t
    {
        // binary (2 operands)
        Mul,
        Add,
        Sub,
        Div,
        Dot,        ///< -> Float
        Min,
        Max,
        Pow,
        Step,
        Mod,
        Cross,      ///< Vec3 x Vec3 -> Vec3
        Reflect,
        // ternary (currently unsupported by the 2-pin Math node)
        Lerp,
        // unary (1 operand; pin 1 ignored)
        Saturate,
        OneMinus,   ///< 1 - x
        Abs,
        Sqrt,
        Floor,
        Fract,
        Sin,
        Cos,
        Normalize,
        Length      ///< -> Float
    };

    /// 常量节点：输出一个 Constant 值。
    class LUX_RESOURCE_PUBLIC ConstantNode : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::Constant;
        ConstantNode();
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<ConstantNode>(*this); }

        float         value[4]   = { 0, 0, 0, 0 };
        EMatValueType value_type = EMatValueType::Vec4;

        /// 设置常量类型并同步输出引脚（编辑器改 arity 时调用）。
        void setType(EMatValueType t);
    };

    /// 输入节点：暴露一个着色输入（uv0 / world_normal / ...）。
    class LUX_RESOURCE_PUBLIC InputNode : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::Input;
        InputNode();
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<InputNode>(*this); }

        EMaterialInput input = EMaterialInput::UV0;
    };

    /// 采样 bindless 纹理（set 2）；`texture_slot` 索引图声明的纹理槽。
    class LUX_RESOURCE_PUBLIC SampleTextureNode : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::SampleTexture;
        SampleTextureNode();
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<SampleTextureNode>(*this); }

        uint32_t texture_slot = 0;
    };

    /// 读一个 per-material 参数（图声明的 param_slot；运行期由 Graph 族 SSBO 提供）。
    /// `param_slot` 索引 MaterialGraph::param_slots；`type` 为该参数类型（用于输出引脚）。
    class LUX_RESOURCE_PUBLIC ParamNode : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::Param;
        explicit ParamNode(EMatValueType type = EMatValueType::Vec4);
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<ParamNode>(*this); }

        uint32_t      param_slot = 0;
        EMatValueType type;

        /// 设置参数类型并同步输出引脚（编辑器改 arity 时调用）。
        void setType(EMatValueType t);
    };

    /// 算术节点。`operand_type` 决定两个操作数引脚的类型（默认 Float）；
    /// 用 setOperandType 设置以表达向量运算（Vec3*Vec3、Vec3·Vec3 等）。
    class LUX_RESOURCE_PUBLIC MathNode : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::Math;
        explicit MathNode(EMathOp op = EMathOp::Mul);
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<MathNode>(*this); }

        EMathOp       op;
        EMatValueType operand_type = EMatValueType::Float;

        /// 设置操作数类型并同步两个输入引脚（及输出引脚，仅作编辑器提示——
        /// lowering 仍从 op 推导真实产出类型，例如 Dot 产出 Float）。
        void setOperandType(EMatValueType t);
    };

    /// 法线贴图解码（rgb -> 切线空间法线）。
    class LUX_RESOURCE_PUBLIC DecodeNormalNode : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::DecodeNormal;
        DecodeNormalNode();
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<DecodeNormalNode>(*this); }
    };

    /// 把切线空间法线变换到世界空间：mat3(T,B,N) * n（用插值的世界 tangent/
    /// bitangent/normal）。配合 DecodeNormal 让法线贴图正确参与光照。
    class LUX_RESOURCE_PUBLIC TbnTransformNode : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::TbnTransform;
        TbnTransformNode();
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<TbnTransformNode>(*this); }
    };

    /// 分量重排 / 截断（如 vec4 纹理 .rgb -> vec3，让纹理可接入 vec3 属性）。
    /// `components[k]` = 输出第 k 通道取源向量的哪个分量（0=x,1=y,2=z,3=w）；
    /// 仅前 arity(out_type) 个有效。默认 vec4 -> vec3 取 .xyz。
    class LUX_RESOURCE_PUBLIC SwizzleNode : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::Swizzle;
        explicit SwizzleNode(EMatValueType source_type = EMatValueType::Vec4,
                             EMatValueType out_type    = EMatValueType::Vec3);
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<SwizzleNode>(*this); }

        EMatValueType source_type;
        EMatValueType out_type;
        uint8_t       components[4] = { 0, 1, 2, 3 };

        /// 设置源/输出类型并同步引脚（输入引脚类型权威 —— resolveInput 据此类型检查）。
        void setTypes(EMatValueType source_type, EMatValueType out_type);
    };

    /// 组装：把 arity(out_type) 个标量分量拼成一个向量（vec3(x,y,z) 等）。
    /// 输入引脚均为 Float，数量 = 输出向量的分量数。Swizzle 的对偶。
    class LUX_RESOURCE_PUBLIC ConstructNode : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::Construct;
        explicit ConstructNode(EMatValueType out_type = EMatValueType::Vec3);
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<ConstructNode>(*this); }

        EMatValueType out_type;
    };

    /// 终端节点：每个 EMaterialAttribute 一个输入引脚（按契约顺序）。
    class LUX_RESOURCE_PUBLIC OutputSurfaceNode : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::OutputSurface;
        OutputSurfaceNode();
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<OutputSurfaceNode>(*this); }
    };

} // namespace lux::rdesc

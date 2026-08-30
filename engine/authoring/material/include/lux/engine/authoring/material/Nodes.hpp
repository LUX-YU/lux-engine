#pragma once
// =============================================================================
//  Nodes.hpp  —  Concrete material nodes (each carries its own payload)
// -----------------------------------------------------------------------------
//  Each node's constructor is responsible for setting up its own input/output
//  pins per the contract (see Node.cpp). Getting the basic skeleton working only
//  needs ConstantNode + OutputSurfaceNode; the rest exist as scaffolding for
//  future expansion.
// =============================================================================

#include <memory>

#include <lux/engine/authoring/material/Node.hpp>

namespace lux::authoring::material
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

    /// Constant node: outputs a single constant value.
    class LUX_ENGINE_AUTHORING_MATERIAL_PUBLIC ConstantNode : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::Constant;
        ConstantNode();
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<ConstantNode>(*this); }

        float         value[4]   = { 0, 0, 0, 0 };
        EMatValueType value_type = EMatValueType::Vec4;

        /// Sets the constant's type and updates the output pin to match (called
        /// by the editor when it changes the arity).
        void setType(EMatValueType t);
    };

    /// Input node: exposes a single shading input (uv0 / world_normal / ...).
    class LUX_ENGINE_AUTHORING_MATERIAL_PUBLIC InputNode : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::Input;
        InputNode();
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<InputNode>(*this); }

        EMaterialInput input = EMaterialInput::UV0;
    };

    /// Samples a bindless texture (set 2); `texture_slot` indexes the texture
    /// slots declared by the graph.
    class LUX_ENGINE_AUTHORING_MATERIAL_PUBLIC SampleTextureNode : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::SampleTexture;
        SampleTextureNode();
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<SampleTextureNode>(*this); }

        uint32_t texture_slot = 0;
    };

    /// Reads a per-material parameter (a param_slot declared by the graph;
    /// supplied at runtime by the Graph-family SSBO). `param_slot` indexes
    /// MaterialGraph::param_slots; `type` is that parameter's type (used for the
    /// output pin).
    class LUX_ENGINE_AUTHORING_MATERIAL_PUBLIC ParamNode : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::Param;
        explicit ParamNode(EMatValueType type = EMatValueType::Vec4);
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<ParamNode>(*this); }

        uint32_t      param_slot = 0;
        EMatValueType type;

        /// Sets the parameter's type and updates the output pin to match (called
        /// by the editor when it changes the arity).
        void setType(EMatValueType t);
    };

    /// Arithmetic node. `operand_type` determines the type of both operand pins
    /// (Float by default); use setOperandType to express vector operations
    /// (Vec3*Vec3, Vec3·Vec3, etc.).
    class LUX_ENGINE_AUTHORING_MATERIAL_PUBLIC MathNode : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::Math;
        explicit MathNode(EMathOp op = EMathOp::Mul);
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<MathNode>(*this); }

        EMathOp       op;
        EMatValueType operand_type = EMatValueType::Float;

        /// Sets the operand type and updates both input pins to match (and the
        /// output pin, but only as an editor hint — lowering still derives the
        /// real result type from `op`; e.g. Dot always produces Float).
        void setOperandType(EMatValueType t);
    };

    /// Normal-map decode (rgb -> tangent-space normal).
    class LUX_ENGINE_AUTHORING_MATERIAL_PUBLIC DecodeNormalNode : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::DecodeNormal;
        DecodeNormalNode();
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<DecodeNormalNode>(*this); }
    };

    /// Transforms a tangent-space normal into world space: mat3(T,B,N) * n
    /// (using the interpolated world-space tangent/bitangent/normal). Pairs with
    /// DecodeNormal so normal maps participate correctly in lighting.
    class LUX_ENGINE_AUTHORING_MATERIAL_PUBLIC TbnTransformNode : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::TbnTransform;
        TbnTransformNode();
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<TbnTransformNode>(*this); }
    };

    /// Component reshuffle / truncation (e.g. a vec4 texture's .rgb -> vec3, so
    /// a texture can feed a vec3 attribute). `components[k]` = which component of
    /// the source vector (0=x,1=y,2=z,3=w) feeds output channel k; only the first
    /// arity(out_type) entries are used. Defaults to vec4 -> vec3 via .xyz.
    class LUX_ENGINE_AUTHORING_MATERIAL_PUBLIC SwizzleNode : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::Swizzle;
        explicit SwizzleNode(EMatValueType source_type = EMatValueType::Vec4,
                             EMatValueType out_type    = EMatValueType::Vec3);
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<SwizzleNode>(*this); }

        EMatValueType source_type;
        EMatValueType out_type;
        uint8_t       components[4] = { 0, 1, 2, 3 };

        /// Sets the source/output types and updates the pins to match (the input
        /// pin's type is authoritative — resolveInput type-checks against it).
        void setTypes(EMatValueType source_type, EMatValueType out_type);
    };

    /// Construct: packs arity(out_type) scalar components into a vector
    /// (vec3(x,y,z), etc.). All input pins are Float, and their count equals the
    /// output vector's component count. The dual of Swizzle.
    class LUX_ENGINE_AUTHORING_MATERIAL_PUBLIC ConstructNode : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::Construct;
        explicit ConstructNode(EMatValueType out_type = EMatValueType::Vec3);
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<ConstructNode>(*this); }

        EMatValueType out_type;
    };

    /// Terminal node: one input pin per EMaterialAttribute (in contract order).
    class LUX_ENGINE_AUTHORING_MATERIAL_PUBLIC OutputSurfaceNode : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::OutputSurface;
        OutputSurfaceNode();
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<OutputSurfaceNode>(*this); }
    };

} // namespace lux::authoring::material
